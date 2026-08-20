================================================================
 SAVIOR'S PATCH
 for Lightning Returns: Final Fantasy XIII (Steam)
 Version 1.0
================================================================

This mod reduces stutters, improves the framerate of the game,
and enables additional graphical settings, like higher resolution
shadows, better antialiasing, and ambient occlusion.


----------------------------------------------------------------
 INSTALLATION
----------------------------------------------------------------

 1. Find your game folder.

    In Steam, right-click Lightning Returns, choose
    Manage -> Browse local files.

    It is usually:
    C:\Program Files (x86)\Steam\steamapps\common\
        LIGHTNING RETURNS FINAL FANTASY XIII

 2. drop every file from this archive into that folder.

 3. Start the game.

That is the whole installation. The stutter and framerate fixes
are already working; the new graphics options are waiting for you
in the game's own menu.


----------------------------------------------------------------
 WHERE ARE THE SETTINGS?
----------------------------------------------------------------

In the game's own menu bar, under Graphics, the same menu the
game already had. There is no separate window to open.


New entries you will find under Graphics:

 - Texture Filtering ... now goes up to 16x (the game stopped
                         at 8x, and its "Standard" setting
                         turned filtering off completely)
 - Mip LOD Bias ....... fixes shimmering on distant ground
 - Ambient Occlusion .. SSAO or HBAO+, with a tuning panel
 - Shadows ............ resolution up to 8192
 - Shadow Distance .... up to 300%
 - MSAA ............... up to 8x
 - SSAA ............... up to 2x supersampling
 - FXAA ............... the game's built-in blur filter, now
                        switchable
 - Frame Rate ......... 30 / 60 / unlimited
 - VSync .............. on or off

Under the "Other" menu you will also find a frame time graph, a
status panel showing what is actually applied, and a button to
reset every setting back to default if you get lost.


----------------------------------------------------------------
 UNINSTALLING
----------------------------------------------------------------

Delete "dinput8.dll" from the game folder. That is all, the
game goes straight back to normal.

You can also delete "SaviorsPatch.ini" and "SaviorsPatch.log"
from the same folder if you want no trace left behind.



----------------------------------------------------------------
 SOMETHING WRONG?
----------------------------------------------------------------

Please include SaviorsPatch.log with your report. It is written
fresh every time you play, it names the exact version you are
running, and it records crashes - a report with the log attached
can usually be acted on, and one without it usually cannot.

If the game will not start at all, delete dinput8.dll to confirm
the mod is the cause before reporting it.


----------------------------------------------------------------
 LICENSE
----------------------------------------------------------------

GNU General Public License v3. This mod is free software and
comes with no warranty. Source code is available.
