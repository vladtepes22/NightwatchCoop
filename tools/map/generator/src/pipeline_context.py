from pathlib import Path
GAME=GAME_DATA=STAGE=BASE=MARKERS=VECTOR=GATES=NAMES=REPORTS=None
def configure(game:Path,stage:Path):
    global GAME,GAME_DATA,STAGE,BASE,MARKERS,VECTOR,GATES,NAMES,REPORTS
    GAME=game;GAME_DATA=game/'dvdroot_ps4';STAGE=stage
    BASE=stage/'base';MARKERS=stage/'markers';VECTOR=stage/'vector'
    GATES=stage/'verified-gates.json';NAMES=stage/'names';REPORTS=stage/'reports'
    for path in (BASE,MARKERS,VECTOR,NAMES,REPORTS):path.mkdir(parents=True,exist_ok=True)
