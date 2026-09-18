================================================================
 SAVIOR'S PATCH
 for Lightning Returns: Final Fantasy XIII (Steam)
 Version 1.1.3
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

 - Resolution .......... now goes up to 8K
 - Texture Filtering ... now goes up to 16x (the game stopped
                         at 8x, and its "Standard" setting
                         turned filtering off completely)
 - Mip LOD Bias ....... fixes shimmering on distant ground
 - Ambient Occlusion .. off, SSAO ,or HBAO+, with a tuning panel
 - Shadows ............ resolution up to 8192
 - Shadow Distance .... up to 300%
 - Shadow Softness .... Off, On, or PCSS (contact-hardening: 
			sharp near the caster, softer 
			further away). "Tuning Panel"
                        opens an in-game panel for either mode.
 - NPC Spawning .......
   Distance ........... Default, or Extended (townsfolk appear
                        from about 2.5x further away)
			/!\ As NPCs spawning is a cause of 
			stuttering this mod tries to mitigate,
			it could reintroduce small hitches, 
			try it out and revert if it causes issues.
			The game has a fixed budget of scene
			actors; in the very densest crowds a few
			background walkers step out while it is
			short and come back afterwards.
 - MSAA ............... up to 8x
 - SSAA ............... up to 2x supersampling
 - FXAA ............... the game's built-in blur filter, now
                        switchable
 - Frame Rate ......... 30 / 60 / unlimited (unlimited is 
			untested and will probably cause
			scripting issues)
 - VSync .............. on or off

Under the "Other" menu you will also find a frame time graph, a
status panel showing what is actually applied, and a button to
reset every setting back to default if you get lost.

The Ambient Occlusion and Shadow tuning panels each have a mode
selector at the top and a Reset button that only resets the mode
currently selected, so trying PCSS never loses your AO numbers.


----------------------------------------------------------------
 NEW IN 1.1.3
----------------------------------------------------------------

 - Fixed a crash with NPC Spawning Distance set to Extended in
   dense crowds (reported in Yusnaan's Reveler's Quarter when
   Flanitors turn hostile). The game has a hard budget of 144
   scene actors and crashed when Extended plus a fight used them
   all; the mod now keeps a reserve free (NpcActorReserve in
   SaviorsPatch.ini, default 32) by thinning the random crowd
   first, and only ever down to the game's own numbers.
 - Existing settings files carry over; the new key appears with
   its default on the next settings change.

----------------------------------------------------------------
 NEW IN 1.1
----------------------------------------------------------------

 - Resolutions up to 8K are now supported
 - Shadow Softness: Off (default game's smoothness) / On / PCSS
 - Shadows no longer "pop" to a lower resolution when the camera
   crosses certain angles relative to the sun. The game switched
   between two shadow projections mid-pan; the mod now keeps the
   stable one.
 - NPC Spawning Distance (Graphics menu): optional, off by
   default. Placed NPCs pop in at 200 units instead of 80, and
   the random crowd is sized for the larger area.
 - In-game Shadow tuning panel, matching the Ambient Occlusion
   one. Both panels got a mode selector and per-mode reset.
 - Existing settings files are kept: the new options simply
   appear with their defaults.


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
comes with no warranty. Source code:
https://github.com/Dendonflo/LR-Saviors-Patch
