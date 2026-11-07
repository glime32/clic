---
name: clic
description: Piloter la souris sur le bureau KDE Plasma (Wayland) de ce PC et voir l'écran — cliquer, double-cliquer, clic droit, glisser, molette, survoler, faire apparaître la barre des tâches auto-masquée — avec des captures annotées d'une grille de coordonnées pour viser au pixel près. Utilise ce skill dès qu'il faut interagir avec une interface graphique ou vérifier à l'écran un changement d'UI sur cette machine : tester un widget/plasmoïde de la barre, ouvrir un menu ou un popup, appuyer sur un bouton d'une appli, contrôler qu'un clic fait bien ce qu'il faut — même si l'utilisateur ne dit pas « clic » (« teste-le », « vérifie que ça marche », « essaie de déplier le widget », « click the button », « check it on screen »).
---

# clic — souris et captures d'écran (KDE Plasma, Wayland)

Deux commandes, dans `~/.local/bin` :

- **`clic`** : souris virtuelle. Le pointeur va **exactement** au pixel demandé (périphérique absolu, sans accélération). ~1 ms par commande, ~0,3 s pour le tout premier appel (le démon se lance seul, puis s'arrête après 15 min sans usage).
- **`clic-shot`** : capture plein écran (~0,3 s) avec une **grille de coordonnées** ; affiche le chemin du PNG, à lire avec l'outil Read.

Si `clic` est introuvable ou répond « démon injoignable » : lancer `sh ~/.claude/skills/clic/scripts/install.sh` (compile et installe, aucun service à activer).

## Coordonnées

Ce sont les **pixels réels de la capture plein écran** (par exemple 2880×1800 pour un écran mis à l'échelle à 160 %). Les nombres jaunes de la grille sont directement les valeurs à donner à `clic`. Ne jamais deviner une position : la lire sur une capture fraîche, car les éléments bougent (la zone de notification change de largeur, les fenêtres se déplacent).

## Boucle de travail

1. **Vue d'ensemble** : `clic-shot` → lire l'image (réduite à ~1440 px, grille tous les 200 px).
2. **Zoom sur la cible** : `clic-shot -r X Y LARGEUR HAUTEUR` → zone agrandie avec une grille fine. `-r` réutilise la même capture : plusieurs zooms sans recapturer.
3. **Agir** : `clic click X Y`. Enchaîner plusieurs actions dans **un seul appel** (plus rapide, et l'état de l'écran ne change pas entre deux appels).
4. **Vérifier** : nouvelle capture de la zone concernée, dans la **même commande Bash** que l'action (`clic … && clic-shot …`), avant que l'écran ne change (barre qui se remasque, infobulle…).

Viser le centre de l'élément, pas son bord. Pour un résultat lent (popup, appli qui s'ouvre), ajouter `wait 300` à `wait 1000` avant la capture.

## Commandes `clic`

| Commande | Effet |
|---|---|
| `move X Y` | déplacer (survol) |
| `click X Y [left\|right\|middle]` | clic (gauche par défaut) |
| `dclick X Y` / `rclick X Y` | double-clic / clic droit |
| `down X Y [b]` / `up X Y [b]` | appuyer / relâcher |
| `drag X1 Y1 X2 Y2` | glisser-déposer (gauche), trajet progressif |
| `scroll X Y N` | molette, N > 0 vers le bas, N < 0 vers le haut |
| `hscroll X Y N` | molette horizontale, N > 0 vers la droite |
| `wait MS` | pause (max 60 000) |
| `ping` / `quit` | vérifier le démon / l'arrêter |

Exemple : `clic move 1440 1799 wait 300 click 2450 1759 wait 400`

Une cible **sur un bord de l'écran** (x = 0 ou 2879, y = 0 ou 1799) déclenche automatiquement la « poussée » que KWin exige pour activer un bord (barre auto-masquée, coins actifs) : ~0,2 s de plus.

## `clic-shot`

```
clic-shot                    écran entier réduit, grille 200 px
clic-shot X Y L H            zone agrandie (jusqu'à ×4), grille auto
  -r     réutiliser la dernière capture      -z F    zoom imposé (ex. 1, 3)
  -g P   pas de grille en px (0 = aucune)    -o F    fichier de sortie
```

Les images vont dans `$XDG_RUNTIME_DIR/clic/` (effacées au bout d'une heure). Pour comparer avant/après, empiler deux captures : `magick A.png B.png -append AB.png`.

## Exemple : barre des tâches auto-masquée en bas d'un écran 2880×1800

À adapter à ton écran (relire les coordonnées sur une capture).

- **L'afficher** : `move 1440 1799 wait 300` (pointeur contre le bord du bas).
- Elle **se remasque** dès que le pointeur n'est plus dessus, et aussi quand un widget change de taille sous le pointeur immobile. Donc **refaire apparaître la barre avant chaque clic** sur elle, dans le même appel :
  ```
  clic move 1440 1799 wait 300 click X 1759 wait 400 move 1440 1799 wait 300 move X 1765 wait 200 && clic-shot 1900 1720 980 80
  ```
  (la 2e révélation sert uniquement à la capturer pour vérifier).
- Les icônes et widgets de la barre sont vers **y ≈ 1759** ; relire leurs x sur une capture à chaque fois.
- Clic sur une zone où la barre n'est plus affichée = clic dans la fenêtre du dessous : toujours révéler juste avant.

## Précautions

- C'est le **vrai pointeur** de l'utilisateur, qui peut être en train de s'en servir. Rester bref, ne pas enchaîner des dizaines d'essais à l'aveugle, et le prévenir quand on va cliquer.
- Ne jamais cliquer sur une action destructive ou engageante (supprimer, envoyer, payer, accepter une demande d'autorisation ou de sécurité) sans son accord explicite.
- Survoler fait apparaître des infobulles qui peuvent cacher la cible : déplacer le pointeur ailleurs avant une capture si besoin.
- Pas de clavier (saisie de texte) pour l'instant.

## Dépannage

- **Rien ne bouge** : `clic ping` ; si le démon est bloqué, `clic quit` (le prochain appel le relance).
- **Clic décalé** après branchement d'un écran ou changement de résolution / d'échelle : `clic quit` (taille et échelle relues au relancement). Un seul écran a été testé.
- **Barre qui n'apparaît pas** : vérifier qu'aucune fenêtre n'est en plein écran (KWin bloque alors les bords).
- **Modifier l'outil** : éditer `src/clic.c` ou `scripts/`, puis relancer `scripts/install.sh`.
