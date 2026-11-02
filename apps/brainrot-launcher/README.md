# Brainrot Launcher

App grid for the Nokia 216: lists `Apps\*.vxp` on the memory card and gives each app a
pixel-art character. D-pad / 2468 move, OK/5 open, RSK exit.

Sprites come from `tools/brainrot_art.py` (needs Pillow), compiled in as `src/sprites.h`.

    make APP_FILE_NAME=Launcher PYTHON=python
