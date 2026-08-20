================================================================
 SAVIOR'S PATCH
 for Lightning Returns: Final Fantasy XIII (Steam)
 Version 1.0
================================================================

This mod reduces stutters, improves the framerate of the game,
and enables additional graphical settings, like higher resolution
shadows, better antialiasing, and ambient occlusion.


----------------------------------------------------------------
 REQUIREMENTS
----------------------------------------------------------------

 - Lightning Returns: Final Fantasy XIII, Steam version
 - Windows

Nothing else. No frameworks, no script extenders, no launcher.


----------------------------------------------------------------
 INSTALLATION
----------------------------------------------------------------

 1. Find your game folder.

    In Steam, right-click Lightning Returns, choose
    Manage -> Browse local files.

    It is usually:
    C:\Program Files (x86)\Steam\steamapps\common\
        LIGHTNING RETURNS FINAL FANTASY XIII

 2. Copy "dinput8.dll" from this archive into that folder.

    It goes right next to LRFF13.exe, not in a subfolder.

 3. Start the game.

That is the whole installation. The stutter and framerate fixes
are already working; the new graphics options are waiting for you
in the game's own menu.


----------------------------------------------------------------
 WHERE ARE THE SETTINGS?
----------------------------------------------------------------

In the game's own menu bar, under Graphics - the same menu the
game already had. There is no separate window to open.

IMPORTANT: that menu bar only exists in WINDOWED mode. If you
play fullscreen, switch to windowed, set your options, then
switch back. Your settings are saved and stay applied.

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

Delete "dinput8.dll" from the game folder. That is all - the
game goes straight back to normal.

You can also delete "SaviorsPatch.ini" and "SaviorsPatch.log"
from the same folder if you want no trace left behind.


----------------------------------------------------------------
 GOOD TO KNOW
----------------------------------------------------------------

MSAA only turns on when your in-game resolution matches your
desktop resolution. The game runs borderless, and below your
desktop resolution MSAA silently does nothing. If MSAA seems to
have no effect, this is why. SSAA works at any resolution.

If you force anisotropic filtering or clamp LOD bias in the
NVIDIA or AMD control panel, your driver overrides this mod.
Set those to "Application-controlled" to let the mod's settings
through.

Other mods: this mod uses the name dinput8.dll. It does not
conflict with the HD GUI mod (which uses version.dll) or with
ReShade (which normally uses d3d9.dll). Only one mod can use any
given name, so if something else in your folder is already called
dinput8.dll, you will have to choose between them.

Two files appear next to the game after the first launch:
 - SaviorsPatch.ini ... your settings
 - SaviorsPatch.log ... a log of the last session


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
