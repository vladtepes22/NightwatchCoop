"""Read-only in-memory Soulstruct adapter for this isolated extraction pipeline.

Avoids Soulstruct's general editor configuration creation and log-file writes.
Only its Binder/DCX/FMG readers are used; unrelated games/tools are disabled.
"""
from pathlib import Path
import pipeline_context as ctx
DES_PATH=DESR_PATH=PTDE_PATH=DSR_PATH=DS2_PATH=DS2_SOTFS_PATH=DS3_PATH=SEKIRO_PATH=ELDEN_RING_PATH=ctx.STAGE/'unsupported-game'
BB_PATH=ctx.GAME_DATA
PARAMDEX_PATH=ctx.STAGE/'unused-paramdex'
LOG_PATH=None
CONSOLE_LOG_LEVEL='WARNING'
FILE_LOG_LEVEL='CRITICAL'
def GET_CONFIG():return {'AUTO_SETUP_LOG':False,'BB_PATH':BB_PATH,'LOG_PATH':None}
def SET_CONFIG(**kwargs):raise RuntimeError('Atlas extraction does not persist global Soulstruct editor settings.')
