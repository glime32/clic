// clic — souris virtuelle pour KDE Plasma Wayland (uinput), pilotée par un socket.
//
// Le pointeur est un périphérique ABSOLU (comme la « tablette » d'une VM) : une position
// donnée en pixels d'écran est atteinte exactement, sans accélération de la souris.
// Le démon garde le périphérique ouvert : une commande prend ~1 ms. Le client le lance lui-même
// au premier appel (~0,5 s) ; il s'arrête seul après 15 min sans usage.
//
// Client :  clic CMD [CMD ...]       (coordonnées = pixels de la capture plein écran)
//   move X Y            déplacer
//   click X Y [b]       clic (b = left|right|middle, défaut left)
//   dclick X Y          double-clic
//   rclick X Y          clic droit
//   down X Y [b] / up X Y [b]   appuyer / relâcher
//   drag X1 Y1 X2 Y2    glisser-déposer (bouton gauche)
//   scroll X Y N        molette : N > 0 vers le bas, N < 0 vers le haut
//   hscroll X Y N       molette horizontale : N > 0 vers la droite
//   wait MS             pause (ex. laisser une barre auto-masquée apparaître)
//   ping / quit         vérifier que le démon répond / l'arrêter (seuls sur la ligne)
//   ex. : clic move 1440 1799 wait 400 click 2620 1770
// Démon :   clic daemon W H [S]     (W x H = écran en pixels réels, S = mise à l'échelle)
// Compiler : gcc -O2 -Wall -o clic clic.c -lm

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define IDLE_MS (15 * 60 * 1000)
#define MAXLINE 4096

// K unités de périphérique par pixel réel : assez fin pour que, avec une mise à l'échelle
// (ex. 1,6), on tombe pile sur les bords LOGIQUES de l'écran. KWin n'active une barre
// auto-masquée que si le pointeur est exactement sur la dernière ligne logique.
#define K 20
static int ufd = -1, rfd = -1, W = 0, H = 0;
static double S = 1;   // mise à l'échelle de l'écran (ex. 1.6)

// Pixel réel -> unité du périphérique. Le dernier pixel va sur le dernier pixel LOGIQUE.
static int dev_unit(long p, int full) {
    return p >= full - 1 ? (int)lround((full - S) * K) : (int)(p * K);
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static int run_path(char *buf, size_t n, const char *name) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return -1;
    return snprintf(buf, n, "%s/%s", rt, name) < (int)n ? 0 : -1;
}

static int connect_sock(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (run_path(a.sun_path, sizeof a.sun_path, "clic.sock")) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) return s;
    if (s >= 0) close(s);
    return -1;
}

// ───────────── Périphérique ─────────────
static void emit_to(int fd, int type, int code, int value) {
    struct input_event ie = { .type = type, .code = code, .value = value };
    if (write(fd, &ie, sizeof ie) < 0) {}
}
static void emit(int type, int code, int value) { emit_to(ufd, type, code, value); }
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void move_units(int ux, int uy) {
    // Le noyau ignore une position identique à la précédente : si la vraie souris a bougé
    // entre-temps, le pointeur ne reviendrait pas. On passe donc d'abord juste à côté (1/K px).
    emit(EV_ABS, ABS_X, ux > 0 ? ux - 1 : ux + 1);
    emit(EV_ABS, ABS_Y, uy > 0 ? uy - 1 : uy + 1);
    syn();
    emit(EV_ABS, ABS_X, ux);
    emit(EV_ABS, ABS_Y, uy);
    syn();
}
// Cible sur un bord de l'écran : un pointeur absolu n'y tombe jamais pile (arrondis), or KWin
// n'active un bord (barre auto-masquée…) que si le pointeur y est exactement. On pousse donc
// au-delà avec la souris relative : KWin bloque le pointeur pile sur le bord, comme une vraie
// souris. Au 1er contact KWin recule le pointeur d'1 px ; une 2e poussée 150 à 350 ms plus
// tard active le bord (comme quand on insiste avec la souris) : d'où les deux poussées.
static void push_edges(long x, long y) {
    int dx = x <= 0 ? -200 : x >= W - 1 ? 200 : 0;
    int dy = y <= 0 ? -200 : y >= H - 1 ? 200 : 0;
    if ((!dx && !dy) || rfd < 0) return;
    for (int k = 0; k < 2; k++) {
        sleep_ms(k ? 200 : 5);
        if (dx) emit_to(rfd, EV_REL, REL_X, dx);
        if (dy) emit_to(rfd, EV_REL, REL_Y, dy);
        emit_to(rfd, EV_SYN, SYN_REPORT, 0);
    }
}
static void move_to(long x, long y) { move_units(dev_unit(x, W), dev_unit(y, H)); push_edges(x, y); }

static void button(int code, int pressed) {
    emit(EV_KEY, code, pressed);
    syn();
}

static int create_device(void) {
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) { perror("clic: /dev/uinput"); return -1; }
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(ufd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(ufd, UI_SET_EVBIT, EV_REL);
    ioctl(ufd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(ufd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    // EV_MSC : avec lui, le noyau reconnaît une « souris absolue » (comme celle d'une VM) et
    // ne crée pas de fausse manette /dev/input/js* que des jeux pourraient voir.
    ioctl(ufd, UI_SET_EVBIT, EV_MSC);
    ioctl(ufd, UI_SET_MSCBIT, MSC_SCAN);
    // libinput fait unité * largeur / (max + 1) : avec max = W*K - 1, K unités = 1 pixel réel.
    struct uinput_abs_setup ax = { .code = ABS_X, .absinfo = { .minimum = 0, .maximum = W * K - 1 } };
    struct uinput_abs_setup ay = { .code = ABS_Y, .absinfo = { .minimum = 0, .maximum = H * K - 1 } };
    if (ioctl(ufd, UI_ABS_SETUP, &ax) < 0 || ioctl(ufd, UI_ABS_SETUP, &ay) < 0) { perror("clic: UI_ABS_SETUP"); return -1; }
    struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0xc11c, .version = 1 } };
    snprintf(us.name, sizeof us.name, "clic virtual pointer");
    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) { perror("clic: UI_DEV_CREATE"); return -1; }

    // 2e périphérique, relatif : seulement pour pousser contre les bords (voir push_edges).
    rfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (rfd >= 0) {
        ioctl(rfd, UI_SET_EVBIT, EV_KEY);
        ioctl(rfd, UI_SET_KEYBIT, BTN_LEFT);   // sans bouton, ce ne serait pas reconnu comme souris
        ioctl(rfd, UI_SET_EVBIT, EV_REL);
        ioctl(rfd, UI_SET_RELBIT, REL_X);
        ioctl(rfd, UI_SET_RELBIT, REL_Y);
        struct uinput_setup rs = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0xc11d, .version = 1 } };
        snprintf(rs.name, sizeof rs.name, "clic virtual pointer (edges)");
        if (ioctl(rfd, UI_DEV_SETUP, &rs) < 0 || ioctl(rfd, UI_DEV_CREATE) < 0) { close(rfd); rfd = -1; }
    }
    sleep_ms(250);  // le temps que KWin ajoute le périphérique, sinon les 1ers événements sont perdus
    return 0;
}

// ───────────── Commandes ─────────────
static int btn_code(const char *s) {
    if (!s || !strcmp(s, "left")) return BTN_LEFT;
    if (!strcmp(s, "right")) return BTN_RIGHT;
    if (!strcmp(s, "middle")) return BTN_MIDDLE;
    return -1;
}

static int num(const char *s, long *out) {
    if (!s) return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || end == s) return -1;
    *out = v;
    return 0;
}

#define NEED(n) do { if (i + (n) >= argc) { snprintf(err, errn, "%s : %d nombre(s) attendu(s)", c, (n)); return -1; } } while (0)
#define XY(k) do { if (num(argv[i + (k)], &x) || num(argv[i + (k) + 1], &y) || x < 0 || y < 0 || x >= W || y >= H) { \
        snprintf(err, errn, "%s : coordonnées hors écran (0..%d, 0..%d)", c, W - 1, H - 1); return -1; } } while (0)

static int run(int argc, char **argv, char *err, size_t errn) {
    for (int i = 0; i < argc; i++) {
        const char *c = argv[i];
        long x, y, x2, y2, n;
        if (!strcmp(c, "move")) {
            NEED(2); XY(1); move_to(x, y); i += 2;
        } else if (!strcmp(c, "click") || !strcmp(c, "dclick") || !strcmp(c, "rclick") || !strcmp(c, "down") || !strcmp(c, "up")) {
            NEED(2); XY(1); i += 2;
            int b = BTN_LEFT;
            if (!strcmp(c, "rclick")) b = BTN_RIGHT;
            else if (i + 1 < argc && btn_code(argv[i + 1]) >= 0) b = btn_code(argv[++i]);
            move_to(x, y);
            sleep_ms(10);
            if (!strcmp(c, "down")) { button(b, 1); continue; }
            if (!strcmp(c, "up")) { button(b, 0); continue; }
            for (int k = 0; k < (!strcmp(c, "dclick") ? 2 : 1); k++) {
                if (k) sleep_ms(60);
                button(b, 1); sleep_ms(15); button(b, 0);
            }
        } else if (!strcmp(c, "drag")) {
            NEED(4); XY(1); x2 = x; y2 = y; XY(3); i += 4;
            move_to(x2, y2); sleep_ms(10);
            button(BTN_LEFT, 1); sleep_ms(30);
            int ax0 = dev_unit(x2, W), ay0 = dev_unit(y2, H), ax1 = dev_unit(x, W), ay1 = dev_unit(y, H);
            for (int k = 1; k <= 12; k++) {  // trajet progressif : les applis voient un vrai glissement
                emit(EV_ABS, ABS_X, ax0 + (ax1 - ax0) * k / 12);
                emit(EV_ABS, ABS_Y, ay0 + (ay1 - ay0) * k / 12);
                syn(); sleep_ms(12);
            }
            sleep_ms(30); button(BTN_LEFT, 0);
        } else if (!strcmp(c, "scroll") || !strcmp(c, "hscroll")) {
            NEED(3); XY(1);
            if (num(argv[i + 3], &n)) { snprintf(err, errn, "%s : nombre de crans invalide", c); return -1; }
            i += 3;
            move_to(x, y); sleep_ms(10);
            int h = !strcmp(c, "hscroll");
            for (long k = 0; k < labs(n); k++) {
                emit(EV_REL, h ? REL_HWHEEL : REL_WHEEL, h ? (n > 0 ? 1 : -1) : (n > 0 ? -1 : 1));
                syn(); sleep_ms(15);
            }
        } else if (!strcmp(c, "wait")) {
            NEED(1);
            if (num(argv[i + 1], &n) || n < 0 || n > 60000) { snprintf(err, errn, "wait : durée en ms (0..60000)"); return -1; }
            sleep_ms(n); i += 1;
        } else if (!strcmp(c, "ping")) {
            // ne fait rien : sert à démarrer le démon et vérifier qu'il répond
        } else {
            snprintf(err, errn, "commande inconnue : %s", c);
            return -1;
        }
    }
    return 0;
}

// ───────────── Démon ─────────────
static void serve(int lfd) {
    char line[MAXLINE];
    for (;;) {
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        int r = poll(&p, 1, IDLE_MS);
        if (r == 0) return;              // inactif : on quitte, le prochain appel relancera le démon
        if (r < 0) { if (errno == EINTR) continue; return; }
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        struct timeval tv = { 2, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        size_t len = 0;
        ssize_t k;
        while (len < sizeof line - 1 && (k = read(c, line + len, sizeof line - 1 - len)) > 0) {
            len += k;
            if (memchr(line, '\n', len)) break;
        }
        line[len] = 0;
        char *argv[512];
        int argc = 0;
        for (char *t = strtok(line, " \t\r\n"); t && argc < 512; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
        if (argc == 1 && !strcmp(argv[0], "quit")) { dprintf(c, "ok\n"); close(c); return; }
        char err[256] = "";
        const char *reply = run(argc, argv, err, sizeof err) ? err : "ok";
        dprintf(c, "%s\n", reply);
        close(c);
    }
}

static int daemon_main(int argc, char **argv) {
    long w, h;
    if (argc < 4 || num(argv[2], &w) || num(argv[3], &h) || w < 2 || h < 2 || w > 50000 || h > 50000) {
        fprintf(stderr, "usage : clic daemon LARGEUR HAUTEUR [ECHELLE]\n");
        return 2;
    }
    W = w; H = h;
    if (argc >= 5) S = atof(argv[4]);
    if (!(S >= 1 && S <= 4)) S = 1;
    // Un seul démon à la fois (deux clients peuvent le lancer en même temps).
    char lock[108];
    if (run_path(lock, sizeof lock, "clic.lock")) { fprintf(stderr, "clic: XDG_RUNTIME_DIR absent\n"); return 1; }
    int lk = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lk < 0) return 1;
    // Verrou pris : soit un démon tourne (on s'efface), soit l'ancien est en train de quitter
    // (« quit », inactivité) : on lui laisse jusqu'à 2 s.
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = !flock(lk, LOCK_EX | LOCK_NB)); i++) sleep_ms(20);
    if (!locked) return 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    run_path(a.sun_path, sizeof a.sun_path, "clic.sock");
    unlink(a.sun_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 8)) { perror("clic: socket"); return 1; }
    if (create_device()) return 1;
    serve(lfd);
    unlink(a.sun_path);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    if (rfd >= 0) { ioctl(rfd, UI_DEV_DESTROY); close(rfd); }
    return 0;
}

// ───────────── Client ─────────────
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "daemon")) return daemon_main(argc, argv);
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fputs("usage : clic move X Y | click X Y [left|right|middle] | dclick X Y | rclick X Y\n"
              "             | down X Y [b] | up X Y [b] | drag X1 Y1 X2 Y2 | scroll X Y N | hscroll X Y N\n"
              "             | wait MS | ping | quit   (plusieurs commandes à la suite possibles)\n", stderr);
        return 2;
    }
    char msg[MAXLINE];
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        int k = snprintf(msg + len, sizeof msg - len, "%s%s", i > 1 ? " " : "", argv[i]);
        if (k < 0 || (size_t)k >= sizeof msg - len) { fputs("clic: commande trop longue\n", stderr); return 2; }
        len += k;
    }
    if (len + 1 >= sizeof msg) return 2;
    msg[len++] = '\n';

    int s = connect_sock();
    if (s < 0 && !strcmp(argv[1], "quit")) return 0;   // pas de démon : rien à arrêter
    if (s < 0) {
        // Pas de démon : on le lance (détaché, il survit à cette commande), puis on attend son socket.
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
            char path[512];
            snprintf(path, sizeof path, "%s/.local/bin/clic-daemon", getenv("HOME") ? getenv("HOME") : "");
            execl(path, path, (char *)NULL);
            _exit(127);
        }
        for (int i = 0; i < 150 && (s = connect_sock()) < 0; i++) sleep_ms(20);
        if (s < 0) {
            fputs("clic: démon injoignable (relancer ~/.claude/skills/clic/scripts/install.sh ?)\n", stderr);
            return 1;
        }
    }
    if (write(s, msg, len) != (ssize_t)len) { perror("clic: envoi"); return 1; }
    char rep[512];
    ssize_t k, got = 0;
    while (got < (ssize_t)sizeof rep - 1 && (k = read(s, rep + got, sizeof rep - 1 - got)) > 0) got += k;
    rep[got > 0 ? got : 0] = 0;
    if (!strncmp(rep, "ok", 2)) return 0;
    fprintf(stderr, "clic: %s", got > 0 ? rep : "pas de réponse du démon\n");
    return 1;
}
