# Clip attribution

All clips come from Wikimedia Commons. The licence was read from each file's own Commons page (File: page metadata). Originals were downloaded from upload.wikimedia.org.

Common processing for every clip: mono, 48 kHz, MP3 96 kbps, `loudnorm=I=-26:TP=-6:LRA=7`, 50 ms fade in and fade out. Clips already shorter than 25 s were used whole. The extra trim, where one was applied, is given in the Edits column.

Licence URLs: CC0 https://creativecommons.org/publicdomain/zero/1.0/ ; CC BY 4.0 https://creativecommons.org/licenses/by/4.0/ ; CC BY-SA 4.0 https://creativecommons.org/licenses/by-sa/4.0/ ; Public domain = marked "Public domain" on the Commons page (the PD Sounds items were published there as public domain).

The CC BY, CC BY-SA and CC0 items were modified (re-encoded, normalised, faded, trimmed). Under CC BY-SA the adapted clips must stay under the same licence if redistributed.

| File | What | Original title | Author / uploader | Licence | Source page | Edits |
|---|---|---|---|---|---|---|
| cat_meow_1.mp3 | Siamese cat meow | Meow of a Siamese cat - freemaster2.wav | freemaster2 (from Freesound) | CC0 | https://commons.wikimedia.org/wiki/File:Meow_of_a_Siamese_cat_-_freemaster2.wav | whole file |
| cat_meow_2.mp3 | Cat meowing to be let out | Meow of a pleading cat.oga | Heismark | Public domain | https://commons.wikimedia.org/wiki/File:Meow_of_a_pleading_cat.oga | whole file |
| cat_purr_1.mp3 | Purring cat | Purring cat.oga | Mysid | Public domain | https://commons.wikimedia.org/wiki/File:Purring_cat.oga | whole file |
| cat_purr_2.mp3 | Purring cat (Bertie) | Purring cat bertie.ogg (PD Sounds, "Purring Cat - Bertie") | jim_mowatt | Public domain | https://commons.wikimedia.org/wiki/File:Purring_cat_bertie.ogg | whole file |
| pc_fan_hum_1.mp3 | Small electric fan hum (stand-in for a PC fan; not a PC recording) | Ventilador de estudio.ogg | Hippocampuz | CC BY-SA 4.0 | https://commons.wikimedia.org/wiki/File:Ventilador_de_estudio.ogg | whole file |
| keyboard_typing_1.mp3 | Typing on an IBM Model M (1986) | Typing - Model M 1986.ogg | Raymangold22 | CC0 | https://commons.wikimedia.org/wiki/File:Typing_-_Model_M_1986.ogg | whole file |
| mouse_click_1.mp3 | Mouse click | Computer Mouse Click.wav | FlowgerWikiCommons | CC0 | https://commons.wikimedia.org/wiki/File:Computer_Mouse_Click.wav | whole file |
| mouse_click_2.mp3 | Mouse single click | Computer mouse single click.ogg | Darklanlan | CC0 | https://commons.wikimedia.org/wiki/File:Computer_mouse_single_click.ogg | whole file |
| rain_window_1.mp3 | Rain against a window | Rain against the window.ogg (PD Sounds "july-rain08") | cori | Public domain | https://commons.wikimedia.org/wiki/File:Rain_against_the_window.ogg | 25 s from 0:20 |
| clock_tick_1.mp3 | Clock ticking | Clock ticking.ogg (PD Sounds "Clock, ticking") | natalie | Public domain | https://commons.wikimedia.org/wiki/File:Clock_ticking.ogg | whole file |
| birds_daytime_1.mp3 | Birdsong in woodland | Birdsong Bourne Woods 2020-04-27 0722.mp3 | Robert EA Harvey | CC BY-SA 4.0 | https://commons.wikimedia.org/wiki/File:Birdsong_Bourne_Woods_2020-04-27_0722.mp3 | 25 s from 0:03 |
| fridge_hum_1.mp3 | Refrigerator hum (door-noise sections cut) | Inside of refrigerator.ogg (PD Sounds) | stephan | Public domain | https://commons.wikimedia.org/wiki/File:Inside_of_refrigerator.ogg | 14.5 s from 0:03 |
| kettle_boiling_1.mp3 | Electric kettle boiling (a gas boiler is faintly audible in the original) | Electric kettle and a gastherme.ogg (PD Sounds) | thore | Public domain | https://commons.wikimedia.org/wiki/File:Electric_kettle_and_a_gastherme.ogg | 25 s from 0:45 |
| door_close_1.mp3 | Door closing | Close door (Gravity Sound).wav | Gravity Sound | CC BY 4.0 | https://commons.wikimedia.org/wiki/File:Close_door_(Gravity_Sound).wav | whole file |
| footsteps_stairs_1.mp3 | Footsteps going up and down a spiral staircase | Pasos en escalera de caracol.ogg | Hippocampuz | CC BY-SA 4.0 | https://commons.wikimedia.org/wiki/File:Pasos_en_escalera_de_caracol.ogg | whole file |

Not included: big-cat/leopard growl (dropped by request).

## Second processing (2026-10-03)

All clips were re-levelled louder after the first batch turned out too quiet for the speakers (the first pass used -26 LUFS):
- Long clips: `loudnorm=I=-18:TP=-2:LRA=7`, mono, 48 kHz, MP3 112 kbps.
- Very short clips (`cat_meow_1`, `door_close_1`, `mouse_click_1`, `mouse_click_2`): peak-normalised to -3 dBFS instead, because loudness normalisation does not suit one-second transients.
- Five ambient clips were turned into seamless loops: the last second is crossfaded (0.8 s, triangular) into the start, so the end joins the beginning without a gap. The old non-loop versions were removed:
  `rain_window_1` -> `rain_window_loop`, `pc_fan_hum_1` -> `pc_fan_hum_loop`, `fridge_hum_1` -> `fridge_hum_loop`,
  `kettle_boiling_1` -> `kettle_boiling_loop`, `birds_daytime_1` -> `birds_daytime_loop`.
  The licence and source of each loop file are those of its original (see the table above). The CC BY-SA items stay CC BY-SA.
