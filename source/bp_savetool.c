/* ---------------------------------------------------------------------------
 * bp_savetool.c -- edit Profile.Save at boot from save.txt.
 *
 * Retargeted from bloonspop_nx, which had the same container but a different
 * password and a much shallower field layout.
 *
 * FORMAT (all of it verified against real saves)
 *     0x00  int32   format version = 1
 *     0x04  int32   record length  = 36
 *     0x08  ...     FileFormatV1: int32 SaveCount, 16-byte Guid,
 *                   int64 DateCreated, int64 DateModified (DateTime.ToBinary)
 *     0x2c  uint64  password version = 3
 *     0x34  24      salt
 *     0x4c  ...     AES-128-CBC( zlib( UTF-8 JSON with BOM ) ), PKCS7
 *
 * Key material is Rfc2898DeriveBytes(password, salt, 10): first GetBytes(16)
 * is the IV, the second is the key -- read out of Constants.GetAes, not
 * assumed. The password is a LITERAL, recovered on hardware by
 * battd_save_probe.c; see password_for().
 *
 * HOW IT EDITS
 * Values are spliced textually into the decoded JSON at a DOTTED PATH, which is
 * the one real change from the Bloons Pop version: almost nothing worth editing
 * here is top-level. Descending by path also keeps the edit precise -- a
 * "money" nested inside some reward entry is never mistaken for
 * resources.money, because the walker only ever looks inside the object the
 * path named.
 *
 * A path the save does not contain is logged and skipped. Nothing is inserted,
 * ever: this can change values the game already writes, not invent fields it
 * would then fail to parse.
 *
 * SAFETY, in the order it happens
 *   1. decode, and bail if the password does not fit (old save, or a game
 *      update that changed the format)
 *   2. apply only the uncommented settings; if nothing actually differs, stop
 *      without writing
 *   3. re-encode, then DECODE THE RESULT AND COMPARE IT byte for byte with
 *      what we meant to write -- a save is not replaced on the strength of an
 *      encoder that was never checked
 *   4. keep the untouched original once as Profile.Save.orig
 *   5. write to a temp file and rename, then commit the SD card
 * MIT.
 * ------------------------------------------------------------------------- */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include "config.h"
#include "bp_savetool.h"
#include "util.h"

const char *bp_game_root(void);

#define MAX_SAVE (4u << 20)

/* ---------------------------------------------------------------------------
 * The editable surface.
 *
 * Bloons Pop's version of this file could substitute at TOP-LEVEL keys only,
 * because that is where its values lived. Almost nothing interesting in BATTD
 * is top-level -- money is resources.money, a hero's level is
 * inventory.towers.<Name>.level -- so this one walks a dotted path instead.
 *
 * KIND: I = integer, B = boolean, S = quoted string.
 * A field is written only if save.txt has it uncommented; a path the save does
 * not contain is logged and skipped, never inserted. ------------------------ */
typedef enum { F_INT, F_BOOL, F_STR } FKind;
typedef struct { const char *key; const char *path; FKind kind; } Field;

static const Field FIELDS[] = {
  /* --- currencies ------------------------------------------------------- */
  { "money",                  "resources.money",                     F_INT  },
  { "gems",                   "resources.gems",                      F_INT  },
  { "xp",                     "resources.xp",                        F_INT  },
  { "crystals",               "resources.crystals",                  F_INT  },
  { "tower_xp",               "resources.genericTowerXpCurrency",    F_INT  },
  { "wish_orb_shards",        "resources.wishOrbs.numShards",        F_INT  },
  /* --- progress --------------------------------------------------------- */
  { "rank",                   "rank",                                F_INT  },
  { "seen_tips",              "progress.seenTips",                   F_INT  },
  { "daily_reward_index",     "progress.dailyRewardIndex",           F_INT  },
  { "tutorial_progress",      "progress.tutorialProgress",           F_STR  },
  { "unlocked_mars",          "progress.hasUnlockedMars",            F_BOOL },
  { "seen_mars_popup",        "progress.seenMarsFirstTimePopup",     F_BOOL },
  { "launched_after_tutorial","appLaunchedAfterTutorial",            F_BOOL },
  /* --- stats and settings ----------------------------------------------- */
  { "played_games",           "stats.playedGames",                   F_INT  },
  { "used_fast_forward",      "stats.usedFastForward",               F_BOOL },
  { "round_auto_play",        "gameSettings.roundAutoPlay",          F_BOOL },
};
#define N_FIELDS ((int)(sizeof FIELDS / sizeof *FIELDS))

/* -1 = not set: leave the game's value alone. */
static long long g_val[N_FIELDS];
static char      g_str[N_FIELDS][64];

/* tower.<Name> -> inventory.towers.<Name>.level
 * adventure.<Name> -> adventureData.adventureProgress.<Name>.isUnlocked
 * Both are open-ended: the name is whatever the save already has, so new heroes
 * and adventures from a game update work without touching this file. */
#define N_DYN 64
typedef struct { char name[48]; long long v; } Dyn;
static Dyn g_tower[N_DYN];   static int g_ntower;
static Dyn g_adv[N_DYN];     static int g_nadv;
static Dyn g_unlock[N_DYN];  static int g_nunlock;
static int g_unlock_all = -1, g_unlock_items = -1;
static long long g_item_count = 1, g_tower_level = -1;
static int g_any;

/* ------------------------------------------------------------------ */
/* save.txt                                                            */
/* ------------------------------------------------------------------ */
/* The whole unlocking section of save.txt: 18 characters, 64 allies, 100
 * weapons, 166 trinkets. One constant, used both by write_template() for a
 * new file and by ADDED[] to append it once to an existing one -- 348 lines
 * duplicated in two places would drift. Ids are checked against ALL_TOWERS
 * and ALL_ITEMS by tools, and those tables are what actually gets written. */
static const char SAVE_TXT_UNLOCKS[] =
  "# --- unlocking ---------------------------------------------------------\n"
  "# The one thing here that ADDS to the save instead of editing it.\n"
  "#\n"
  "# The game stores the two kinds separately and mixing them up crashes it:\n"
  "#   inventory.towers -- the 18 playable CHARACTERS\n"
  "#   inventory.items  -- 64 allies, 100 weapons, 166 trinkets\n"
  "# ALLIES ARE ITEM CARDS, NOT TOWERS. unlock.<Name> works out which list a\n"
  "# name belongs to; a name in neither is reported and skipped. HotDogKnights\n"
  "# and MarshmallowKids are summoned units and are in neither.\n"
  "#\n"
  "# The name is the game's internal id, taken from the model definitions in\n"
  "# the towers bundle; the comment is the name you see in game. They often\n"
  "# differ in ways you would not guess -- BMO is \"Bmo\", BOM-MO is \"BomMo\",\n"
  "# MOAP is \"Moap\", Liquefier is \"Liquifier\" -- so search the comments.\n"
  "#\n"
  "#unlock_all_towers = true      # all 18 characters\n"
  "#unlock_all_items = true       # all 330 allies, weapons and trinkets\n"
"#\n"
"# --- how many, and what level ------------------------------------------\n"
"# item_count = N    hold N copies of every item unlocked above (max 10).\n"
"#                   There is no quantity field in the save: N copies is N\n"
"#                   entries, which is how the game lets you equip the same\n"
"#                   trinket to several characters. Existing copies count,\n"
"#                   so raising this tops up instead of duplicating.\n"
"# tower_level = N   set every character you own to level N. The game caps\n"
"#                   this at 10 (towerMaxLevel); higher is clamped.\n"
"# max_everything    all characters, all items, 10 copies each, level 10.\n"
"#item_count = 1\n"
"#tower_level = 10\n"
"#max_everything = true\n"
  "\n"
  "# --- characters (18) ----------------------------------------\n"
  "#unlock.Finn                            = true   # Finn\n"
  "#unlock.Jake                            = true   # Jake\n"
  "#unlock.Max                             = true   # Max\n"
  "#unlock.Bubblegum                       = true   # Princess Bubblegum\n"
  "#unlock.IceKing                         = true   # Ice King\n"
  "#unlock.CaptainCassie                   = true   # Captain Cassie\n"
  "#unlock.Marceline                       = true   # Marceline\n"
  "#unlock.Sam                             = true   # Sam\n"
  "#unlock.FlamePrincess                   = true   # Flame Princess\n"
  "#unlock.C4Charlie                       = true   # C4 Charlie\n"
  "#unlock.Sai                             = true   # Sai\n"
  "#unlock.SuperMonkey                     = true   # Supermonkey\n"
  "#unlock.WarriorFinn                     = true   # Dungeon Finn\n"
  "#unlock.SerenadingJake                  = true   # Tuxedo Jake\n"
  "#unlock.JuggernautMax                   = true   # Juggernaut Max\n"
  "#unlock.WarmasterBubblegum              = true   # Warrior Bubblegum\n"
  "#unlock.CommanderCassie                 = true   # Commander Cassie\n"
  "#unlock.MarcelineTheVampireHunter       = true   # Hunter Marceline\n"
  "\n"
  "# --- allies (64) --------------------------------------------\n"
  "#unlock.Abracadaniel                    = true   # Abracadaniel\n"
  "#unlock.AncientPsychicTandemWarElephant = true   # Ancient Psychic Tandem War Elephant\n"
  "#unlock.BananaAirCorps                  = true   # Banana Air Corps\n"
  "#unlock.BananaGuards                    = true   # Banana Guards\n"
  "#unlock.BananaMan                       = true   # Banana Man\n"
  "#unlock.BettyGrof                       = true   # Betty Grof\n"
  "#unlock.Billy                           = true   # Billy\n"
  "#unlock.Bmo                             = true   # BMO\n"
  "#unlock.BoomerangMonkey                 = true   # Boomerang Monkey\n"
  "#unlock.BusinessMen                     = true   # Business Men\n"
  "#unlock.CinnamonBun                     = true   # Cinnamon Bun\n"
  "#unlock.Clarence                        = true   # Clarence\n"
  "#unlock.Cobra                           = true   # COBRA\n"
  "#unlock.DartMonkey                      = true   # Dart Monkey\n"
  "#unlock.DirtBeerGuy                     = true   # Dirt Beer Guy\n"
  "#unlock.DrMonkey                        = true   # Dr Monkey\n"
  "#unlock.ElfMonkey                       = true   # Elf Monkey\n"
  "#unlock.FlameKing                       = true   # Flame King\n"
  "#unlock.GhostPrincess                   = true   # Ghost princess\n"
  "#unlock.GrassyWizard                    = true   # Grassy Wizard\n"
  "#unlock.GrobGobGlobGrod                 = true   # Grob Gob Glob Grod\n"
  "#unlock.Gumbald                         = true   # Gumbald\n"
  "#unlock.Gunter                          = true   # Gunter\n"
  "#unlock.HolidayBmo                      = true   # Holiday BMO\n"
  "#unlock.HunsonAbadeer                   = true   # Hunson Abadeer\n"
  "#unlock.HuntressWizard                  = true   # Huntress Wizard\n"
  "#unlock.IceMonkey                       = true   # Ice Monkey\n"
  "#unlock.KingOfOoo                       = true   # King of Ooo\n"
  "#unlock.LadyRainicorn                   = true   # Lady Rainicorn\n"
  "#unlock.LaserButterfly                  = true   # Laser Butterfly\n"
  "#unlock.Lemonhope                       = true   # Lemonhope\n"
  "#unlock.LumpySpacePrincess              = true   # Lumpy Space Princess\n"
  "#unlock.Maja                            = true   # Maja\n"
  "#unlock.MartianTransport                = true   # Martian Transport\n"
  "#unlock.Martin                          = true   # Martin\n"
  "#unlock.Minipults                       = true   # Minipults\n"
  "#unlock.Moe                             = true   # Moe\n"
  "#unlock.MonkeyApprentice                = true   # Monkey Apprentice\n"
  "#unlock.BananaFarmer                    = true   # Monkey Farmer\n"
  "#unlock.MusclePrincess                  = true   # Muscle Princess\n"
  "#unlock.Neptr                           = true   # NEPTR\n"
  "#unlock.PartyGod                        = true   # Party God\n"
  "#unlock.PeppermintButler                = true   # Peppermint Butler\n"
  "#unlock.PirateCrew                      = true   # Pirate Crew\n"
  "#unlock.Rattleballs                     = true   # Rattleballs\n"
  "#unlock.Ricardio                        = true   # Ricardio\n"
  "#unlock.Scorcher                        = true   # Scorcher\n"
  "#unlock.Shoko                           = true   # Shoko\n"
  "#unlock.SlimePrincess                   = true   # Slime Princess\n"
  "#unlock.SniperMonkey                    = true   # Sniper Monkey\n"
  "#unlock.SpaceLards                      = true   # Space Lards\n"
  "#unlock.Squadron                        = true   # Squadron\n"
  "#unlock.StarMan                         = true   # Star Man\n"
  "#unlock.Starchy                         = true   # Starchy\n"
  "#unlock.SuperFans                       = true   # Super Fans\n"
  "#unlock.SusanStrong                     = true   # Susan Strong\n"
  "#unlock.TechnologicalTerror             = true   # Technological Terror\n"
  "#unlock.TinyManticore                   = true   # Tiny Manticore\n"
  "#unlock.TrainBoss                       = true   # Train Boss\n"
  "#unlock.Treetrunks                      = true   # Treetrunks\n"
  "#unlock.VampireKing                     = true   # Vampire King\n"
  "#unlock.WaterNymph                      = true   # Water Nymph\n"
  "#unlock.WildberryPrincess               = true   # Wildberry Princess\n"
  "#unlock.WizardLord                      = true   # Wizard Lord\n"
  "\n"
  "# --- weapons (100) ------------------------------------------\n"
  "#unlock.4DSword                         = true   # 4D sword\n"
  "#unlock.AbracadanielsWand               = true   # Abracadaniel's Wand\n"
  "#unlock.AcousticGuitar                  = true   # Acoustic Guitar\n"
  "#unlock.AxBass                          = true   # Ax Bass\n"
  "#unlock.BallBlamBurglerber              = true   # Ball Blam Burglerber\n"
  "#unlock.Bananarangs                     = true   # Bananarangs\n"
  "#unlock.Banjo                           = true   # Banjo\n"
  "#unlock.BarbedDarts                     = true   # Barbed Darts\n"
  "#unlock.BlessedDart                     = true   # Blessed Dart\n"
  "#unlock.Bloonsbane                      = true   # Bloonsbane\n"
  "#unlock.BomMo                           = true   # BOM-MO\n"
  "#unlock.BombChain                       = true   # Bomb & Chain\n"
  "#unlock.Bomba                           = true   # Bomba\n"
  "#unlock.BreadstickWand                  = true   # Breadstick Wand\n"
  "#unlock.ButterscotchBomb                = true   # Butterscotch Bomb\n"
  "#unlock.CandyBomb                       = true   # Candy Bomb\n"
  "#unlock.CandyCaneShotgun                = true   # Candy Cane Shotgun\n"
  "#unlock.CandyDuckAxe                    = true   # Candy Duck Axe\n"
  "#unlock.CandyHorseTranquilizer          = true   # Candy Horse Tranquilizer\n"
  "#unlock.CandyMicrophone                 = true   # Candy Microphone\n"
  "#unlock.CaptainTreeTrunksCutlass        = true   # Captain Tree Trunks' Cutlass\n"
  "#unlock.CarbBomb                        = true   # Carb Bomb\n"
  "#unlock.CherryBlossomWand               = true   # Cherry Blossom Wand\n"
  "#unlock.CherryBomb                      = true   # Cherry Bomb\n"
  "#unlock.CryoBomb                        = true   # Cryo Bomb\n"
  "#unlock.CyberneticArrow                 = true   # Cybernetic Arrow\n"
  "#unlock.DemonBloodSword                 = true   # Demon Blood Sword\n"
  "#unlock.DevilMonsterBass                = true   # Devil Monster Bass\n"
  "#unlock.DrMonkeysSecretWeapon           = true   # Dr Monkey's Secret Weapon\n"
  "#unlock.DragonFangs                     = true   # Dragon Fangs\n"
  "#unlock.DrillerDarts                    = true   # Driller Darts\n"
  "#unlock.DynamiteStack                   = true   # Dynamite Stack\n"
  "#unlock.ElectrodeGun                    = true   # Electrode Gun\n"
  "#unlock.Excandybur                      = true   # Excandybur\n"
  "#unlock.ExplodingPineapples             = true   # Exploding Pineapples\n"
  "#unlock.FinnSword                       = true   # Finn Sword\n"
  "#unlock.FinnsFlute                      = true   # Finn's Flute\n"
  "#unlock.FireGuitar                      = true   # Fire Guitar\n"
  "#unlock.FireKingdomScepter              = true   # Fire Kingdom Scepter\n"
  "#unlock.FireSword                       = true   # Fire Sword\n"
  "#unlock.FrozenThrowingKnives            = true   # Frozen Throwing Knives\n"
  "#unlock.Fumigator                       = true   # Fumigator\n"
  "#unlock.GarlicBomb                      = true   # Garlic Bomb\n"
  "#unlock.GlobsSword                      = true   # Glob's Sword\n"
  "#unlock.GoldenViola                     = true   # Golden Viola\n"
  "#unlock.GrassSword                      = true   # Grass Sword\n"
  "#unlock.GrassWand                       = true   # Grass Wand\n"
  "#unlock.GrenadeOfGlob                   = true   # Grenade of Glob\n"
  "#unlock.GuntersTaser                    = true   # Gunter's Taser\n"
  "#unlock.GwensFlamethrower               = true   # Gwen's Flamethrower\n"
  "#unlock.HonestyBells                    = true   # Honesty Bells\n"
  "#unlock.JakesSword                      = true   # Jake's Sword\n"
  "#unlock.JakesViola                      = true   # Jake's Viola\n"
  "#unlock.JewelledCabasas                 = true   # Jewelled Cabasas\n"
  "#unlock.JingleBomb                      = true   # Jingle Bomb\n"
  "#unlock.LightningBolts                  = true   # Lightning Bolts\n"
  "#unlock.Liquifier                       = true   # Liquefier\n"
  "#unlock.LiquidPyrotechnicsLauncher      = true   # Liquid Pyrotechnics Launcher\n"
  "#unlock.LunaticBass                     = true   # Lunatic Bass\n"
  "#unlock.MartianBlaster                  = true   # Martian Blaster\n"
  "#unlock.MartianMic                      = true   # Martian Mic\n"
  "#unlock.MilitaryDarts                   = true   # Military Darts\n"
  "#unlock.Moap                            = true   # MOAP\n"
  "#unlock.MouthOrgan                      = true   # Mouth Organ\n"
  "#unlock.MushroomBomb                    = true   # Mushroom Bomb\n"
  "#unlock.NailGun                         = true   # Nail Gun\n"
  "#unlock.NightSword                      = true   # Night Sword\n"
  "#unlock.Nothung                         = true   # Nothung\n"
  "#unlock.PenguinShell                    = true   # Penguin Shell\n"
  "#unlock.PeppermintBattleAxe             = true   # Peppermint Battle Axe\n"
  "#unlock.PhoenixWand                     = true   # Phoenix Wand\n"
  "#unlock.PressureHose                    = true   # Pressure Hose\n"
  "#unlock.RainbowGlitterWand              = true   # Rainbow Glitter Wand\n"
  "#unlock.RazorBats                       = true   # Razor Bats\n"
  "#unlock.RepairedViola                   = true   # Repaired Viola\n"
  "#unlock.RevengeStick                    = true   # Revenge Stick\n"
  "#unlock.RodOfNiceness                   = true   # Rod of Niceness\n"
  "#unlock.RootSword                       = true   # Root Sword\n"
  "#unlock.SassageFlare                    = true   # Sassage Flare\n"
  "#unlock.Scarlet                         = true   # Scarlet\n"
  "#unlock.SilverShurikens                 = true   # Silver Shurikens\n"
  "#unlock.SilverTippedStakes              = true   # Silver Tipped Stakes\n"
  "#unlock.SnakeDarts                      = true   # Snake Darts\n"
  "#unlock.SniperRifle                     = true   # Sniper Rifle\n"
  "#unlock.SoulRedeemerSword               = true   # Soul Redeemer Sword\n"
  "#unlock.SpiderWand                      = true   # Spider Wand\n"
  "#unlock.SplodeyDarts                    = true   # Splodey Darts\n"
  "#unlock.StickyShots                     = true   # Sticky Shots\n"
  "#unlock.TheLover                        = true   # The Lover\n"
  "#unlock.ThievesKatana                   = true   # Thieves' Katana\n"
  "#unlock.ThoughtCannonWand               = true   # Thought Cannon Wand\n"
  "#unlock.Thundersword                    = true   # Thundersword\n"
  "#unlock.TimeBomb                        = true   # Time Bomb\n"
  "#unlock.UnimaginablyAmazingSword        = true   # Unimaginably Amazing Sword\n"
  "#unlock.WandOfDispersement              = true   # Wand of Dispersement\n"
  "#unlock.WebGun                          = true   # Web Gun\n"
  "#unlock.WishyWand                       = true   # Wishy Wand\n"
  "#unlock.WizardLordWand                  = true   # Wizard Lord Wand\n"
  "#unlock.WizardThiefWand                 = true   # Wizard Thief Wand\n"
  "#unlock.XergioksWand                    = true   # Xergiok's Wand\n"
  "\n"
  "# --- trinkets (166) -----------------------------------------\n"
  "#unlock.AbracadanielsHeadband           = true   # Abracadaniel's Headband\n"
  "#unlock.AbrahamLincolnsPenny            = true   # Abraham Lincoln's Penny\n"
  "#unlock.AntiCamoDust                    = true   # Anti-Camo Dust\n"
  "#unlock.AntiGravityToteChamber          = true   # Anti-Gravity Tote Chamber\n"
  "#unlock.Apple                           = true   # Apple\n"
  "#unlock.ApprenticeCap                   = true   # Apprentice Cap\n"
  "#unlock.ArrowOfIce                      = true   # Arrow of Ice\n"
  "#unlock.BabyBlanket                     = true   # Baby Blanket\n"
  "#unlock.BabyTooth                       = true   # Baby Tooth\n"
  "#unlock.BagOfLollies                    = true   # Bag of Lollies\n"
  "#unlock.BakersShard                     = true   # Baker's Shard\n"
  "#unlock.BananaReplicator                = true   # Banana Replicator\n"
  "#unlock.Basketball                      = true   # Basketball\n"
  "#unlock.BeANinja                        = true   # Be a Ninja\n"
  "#unlock.BeauteousWings                  = true   # Beauteous Wings\n"
  "#unlock.BigRedButton                    = true   # Big Red Button\n"
  "#unlock.BlackBowTie                     = true   # Black Bow Tie\n"
  "#unlock.BlazingFeet                     = true   # Blazing Feet\n"
  "#unlock.BloonTrap                       = true   # Bloon Trap\n"
  "#unlock.BmosSkateboard                  = true   # BMO's Skateboard\n"
  "#unlock.BoobooSousa                     = true   # Booboo Sousa\n"
  "#unlock.BottleRocket                    = true   # Bottle Rocket\n"
  "#unlock.BoxOfDirt                       = true   # Box of Dirt\n"
  "#unlock.BrainFood                       = true   # Brain Food\n"
  "#unlock.BranchesOfPalm                  = true   # Branches of Palm\n"
  "#unlock.BubblegumsHair                  = true   # Bubblegum's Hair\n"
  "#unlock.CandyDiveSuit                   = true   # Candy Dive Suit\n"
  "#unlock.CandySeeds                      = true   # Candy Seeds\n"
  "#unlock.CandycornSpear                  = true   # Candycorn Spear\n"
  "#unlock.CaptainTreeTrunksEyepatch       = true   # Captain Tree Trunks' Eyepatch\n"
  "#unlock.CarlTheGem                      = true   # Carl the Gem\n"
  "#unlock.CheesyDog                       = true   # Cheesy Dog\n"
  "#unlock.ClaBlade                        = true   # Cla Blade\n"
  "#unlock.Condiments                      = true   # Condiments\n"
  "#unlock.CosmicGauntlets                 = true   # Cosmic Gauntlets\n"
  "#unlock.Cryojet                         = true   # CryoJet\n"
  "#unlock.CrystalGemApple                 = true   # Crystal Gem Apple\n"
  "#unlock.CrystalMergenceOfDestruction    = true   # Crystal Mergence of Destruction\n"
  "#unlock.CursedIceRing                   = true   # Cursed Ice Ring\n"
  "#unlock.DaggerOfChilledGlass            = true   # Dagger of Chilled Glass\n"
  "#unlock.DarkTempleIdol                  = true   # Dark Temple Idol\n"
  "#unlock.DaveyStache                     = true   # Davey 'Stache\n"
  "#unlock.DeathsDrums                     = true   # Death's Drums\n"
  "#unlock.DemonHeart                      = true   # Demon Heart\n"
  "#unlock.DemonicWishingEye               = true   # Demonic Wishing Eye\n"
  "#unlock.DiveSuit                        = true   # Dive Suit\n"
  "#unlock.DoomGauntlets                   = true   # Doom Gauntlets\n"
  "#unlock.DragonEyes                      = true   # Dragon Eyes\n"
  "#unlock.ElderPlopsScepter               = true   # Elder Plop's Scepter\n"
  "#unlock.ElementalStaff                  = true   # Elemental Staff\n"
  "#unlock.EnchantedBoomerang              = true   # Enchanted Boomerang\n"
  "#unlock.EngineersBlueprints             = true   # Engineers Blueprints\n"
  "#unlock.EyeFlail                        = true   # Eye Flail\n"
  "#unlock.FinnsCrossbow                   = true   # Finn's Crossbow\n"
  "#unlock.FireCrown                       = true   # Fire Crown\n"
  "#unlock.FlowerCrown                     = true   # Flower Crown\n"
  "#unlock.FreezingPotionA                 = true   # Freezing Potion A\n"
  "#unlock.FutureCrystal                   = true   # Future Crystal\n"
  "#unlock.GauntletOfBones                 = true   # Gauntlet of Bones\n"
  "#unlock.GauntletOfTheHero               = true   # Gauntlet of the Hero\n"
  "#unlock.GemmaTheGemstone                = true   # Gemma the Gemstone\n"
  "#unlock.GentleLasers                    = true   # Gentle Lasers\n"
  "#unlock.GiantTranq                      = true   # Giant Tranq\n"
  "#unlock.GlaiveOfTheAncients             = true   # Glaive of the Ancients\n"
  "#unlock.GlassesOfNerdicon               = true   # Glasses of Nerdicon\n"
  "#unlock.GlobsHelmet                     = true   # Glob's Helmet\n"
  "#unlock.Googoomamameter                 = true   # Googoomamameter\n"
  "#unlock.GrapplingCrossbow               = true   # Grappling crossbow\n"
  "#unlock.GraveRing                       = true   # Grave Ring\n"
  "#unlock.Hambo                           = true   # Hambo\n"
  "#unlock.HeartGauntlets                  = true   # Heart Gauntlets\n"
  "#unlock.HollyJollyScarf                 = true   # Holly Jolly Scarf\n"
  "#unlock.HollyJollySweater               = true   # Holly Jolly Sweater\n"
  "#unlock.HorseySoap                      = true   # Horsey Soap\n"
  "#unlock.IceBull                         = true   # Ice Bull\n"
  "#unlock.IceCreamSundae                  = true   # Ice Cream Sundae\n"
  "#unlock.IceCrook                        = true   # Ice Crook\n"
  "#unlock.IcebergBlade                    = true   # Iceberg Blade\n"
  "#unlock.IronHull                        = true   # Iron Hull\n"
  "#unlock.IssueOfBle                      = true   # Issue of Ble\n"
  "#unlock.JamesLuckyCoin                  = true   # James' Lucky Coin\n"
  "#unlock.Jetpack                         = true   # Jetpack\n"
  "#unlock.KingOfOoosSceptre               = true   # King of Ooo's Scepter\n"
  "#unlock.KnifeStormCloud                 = true   # Knife Storm Cloud\n"
  "#unlock.LambRelic                       = true   # Lamb Relic\n"
  "#unlock.Lemonsweets                     = true   # Lemonsweets\n"
  "#unlock.LittleDude                      = true   # Little Dude\n"
  "#unlock.LumpinDeliciousSandwiches       = true   # Lumpin Delicious Sandwiches\n"
  "#unlock.LuteSuit                        = true   # Lute Suit\n"
  "#unlock.MaceStake                       = true   # Mace Stake\n"
  "#unlock.MagicCarpet                     = true   # Magic Carpet\n"
  "#unlock.MagicCoinPurse                  = true   # Magic Coin Purse\n"
  "#unlock.MagicDoorPortal                 = true   # Magic Door Portal\n"
  "#unlock.MagicMansHat                    = true   # Magic Man's Hat\n"
  "#unlock.MagicPowder                     = true   # Magic Powder\n"
  "#unlock.MagicSpanner                    = true   # Magic Spanner\n"
  "#unlock.MargaretsMusicBox               = true   # Margaret's Music Box\n"
  "#unlock.MartianTrackingDevice           = true   # Martian Tracking Device\n"
  "#unlock.MaskOfShadows                   = true   # Mask of Shadows\n"
  "#unlock.MedallionOfBrogends             = true   # Medallion of Brogends\n"
  "#unlock.MindGames                       = true   # Mind Games\n"
  "#unlock.Missile                         = true   # Missile\n"
  "#unlock.MonkeyAcademyDegree             = true   # Monkey Academy Degree\n"
  "#unlock.MonkeyKingsRobe                 = true   # Monkey King's Robe\n"
  "#unlock.MortarHelmet                    = true   # Mortar Helmet\n"
  "#unlock.MysteryCavePick                 = true   # Mystery Cave Pick\n"
  "#unlock.NightVisionXRayGoggles          = true   # Night Vision X-Ray goggles\n"
  "#unlock.NinjaDagger                     = true   # Ninja Dagger\n"
  "#unlock.NinjaHeadband                   = true   # Ninja Headband\n"
  "#unlock.NumbChuks                       = true   # Numb-Chuks\n"
  "#unlock.PaperPlateMask                  = true   # Paper Plate Mask\n"
  "#unlock.PeacockHat                      = true   # Peacock Hat\n"
  "#unlock.PennysDagger                    = true   # Penny's Dagger\n"
  "#unlock.PinkSweater                     = true   # Pink Sweater\n"
  "#unlock.PirateHat                       = true   # Pirate Hat\n"
  "#unlock.PlasmaGoggles                   = true   # Plasma Goggles\n"
  "#unlock.PocketWatch                     = true   # Pocket Watch\n"
  "#unlock.PowerRingOfChill                = true   # Power Ring of Chill\n"
  "#unlock.PowerRingOfDamage               = true   # Power Ring of Damage\n"
  "#unlock.PowerRingOfPierce               = true   # Power Ring of Pierce\n"
  "#unlock.PowerRingOfPoison               = true   # Power Ring of Poison\n"
  "#unlock.PowerRingOfRange                = true   # Power Ring of Range\n"
  "#unlock.PowerRingOfSpeed                = true   # Power Ring of Speed\n"
  "#unlock.PowerRingOfStrength             = true   # Power Ring of Strength\n"
  "#unlock.Powerometer                     = true   # Powerometer\n"
  "#unlock.PrincessPlant                   = true   # Princess Plant\n"
  "#unlock.ProtectionGem                   = true   # Protection Gem\n"
  "#unlock.RCPlane                         = true   # R/C Plane\n"
  "#unlock.Rainicornicopia                 = true   # Rainicornicopia\n"
  "#unlock.RedBowTie                       = true   # Red Bow Tie\n"
  "#unlock.RedCowboyBoots                  = true   # Red Cowboy boots\n"
  "#unlock.RenceHilt                       = true   # Rence Hilt\n"
  "#unlock.RoboMonkeyVisor                 = true   # Robo-Monkey Visor\n"
  "#unlock.RockShirt                       = true   # Rock Shirt\n"
  "#unlock.RoyalMedalForHeroicBravery      = true   # Royal Medal for Heroic Bravery\n"
  "#unlock.SacredSpringScarf               = true   # Sacred Spring Scarf\n"
  "#unlock.ShardOfEverfrost                = true   # Shard of Everfrost\n"
  "#unlock.SilverDagger                    = true   # Silver Dagger\n"
  "#unlock.SirenHat                        = true   # Siren Hat\n"
  "#unlock.SniperBeret                     = true   # Sniper Beret\n"
  "#unlock.SoftPretzels                    = true   # Soft Pretzels\n"
  "#unlock.SoulStone                       = true   # Soul Stone\n"
  "#unlock.SpecialSentientSandwich         = true   # Special Sentient Sandwich\n"
  "#unlock.SpoonOfProsperity               = true   # Spoon of Prosperity\n"
  "#unlock.StoneOfAncientKnowledge         = true   # Stone of Ancient Knowledge\n"
  "#unlock.StoneSkinPotion                 = true   # Stone Skin Potion\n"
  "#unlock.Strawberry                      = true   # Strawberry\n"
  "#unlock.StrikerJonesCap                 = true   # Striker Jones' Cap\n"
  "#unlock.SuperPorp                       = true   # Super Porp\n"
  "#unlock.Taser                           = true   # Taser\n"
  "#unlock.ThiefCrossbow                   = true   # Thief Crossbow\n"
  "#unlock.ThiefKingsDagger                = true   # Thief King's Dagger\n"
  "#unlock.TigerClaw                       = true   # Tiger Claw\n"
  "#unlock.TikiShield                      = true   # Tiki Shield\n"
  "#unlock.TimeTravelMachine               = true   # Time Travel Machine\n"
  "#unlock.TreasureChestKeys               = true   # Treasure Chest Keys\n"
  "#unlock.TreetrunksAppleKnife            = true   # Treetrunk's Apple Knife\n"
  "#unlock.TreetrunksDagger                = true   # Treetrunk's Dagger\n"
  "#unlock.Trident                         = true   # Trident\n"
  "#unlock.UniversalCoin                   = true   # Universal Coin\n"
  "#unlock.VorpalHand                      = true   # Vorpal Hand\n"
  "#unlock.WarningHorn                     = true   # Warning Horn\n"
  "#unlock.WarpaintMud                     = true   # Warpaint Mud\n"
  "#unlock.WhistlingBook                   = true   # Whistling Book\n"
  "#unlock.WindmillDagger                  = true   # Windmill Dagger\n"
  "#unlock.WizardNunchuks                  = true   # Wizard Nunchuks\n";

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[save] could not write %s\n", path); return; }
  fputs(
"# save.txt -- Bloons Adventure Time TD save editing.\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to Profile.Save at EVERY launch, for as long as the line stays\n"
"# uncommented. The original is kept once as Profile.Save.orig.\n"
"#\n"
"# A field the save does not contain is reported in debug.log and skipped --\n"
"# nothing is ever inserted, so this cannot invent data the game will choke on.\n"
"\n"
"# --- currencies ------------------------------------------------------\n"
"#money = 999999\n"
"#gems = 9999\n"
"#xp = 99999\n"
"#crystals = 9999\n"
"#tower_xp = 99999\n"
"#wish_orb_shards = 999\n"
"\n"
"# --- progress --------------------------------------------------------\n"
"#rank = 50\n"
"#seen_tips = 1\n"
"#daily_reward_index = 0\n"
"# tutorial_progress is an enum name. 'Complete' skips the tutorial;\n"
"# 'InitialMission' is where a new profile starts.\n"
"#tutorial_progress = Complete\n"
"#unlocked_mars = true\n"
"#seen_mars_popup = true\n"
"#launched_after_tutorial = true\n"
"\n"
"# --- stats and settings ----------------------------------------------\n"
"#played_games = 100\n"
"#used_fast_forward = true\n"
"#round_auto_play = true\n"
"\n"
"# --- heroes ----------------------------------------------------------\n"
"# tower.<Name> sets that hero's level. The name is whatever the save already\n"
"# holds, so heroes added by a game update work without changing the port.\n"
"# A new profile starts with Finn, Jake and Max.\n"
"# The cap is 10; higher values are clamped.\n"
"#tower.Finn = 10\n"
"#tower.Jake = 10\n"
"#tower.Max = 10\n"
"\n",
    f);
  fputs(SAVE_TXT_UNLOCKS, f);
  fputs(
"\n"
"# --- adventures ------------------------------------------------------\n"
"# adventure.<Name> = true unlocks it. Names in a fresh save:\n"
"#   Tutorial, AppleThief, WinterIsComing, CandyCornered,\n"
"#   MarcelineTheVampireHunter, WizardBattle, LemonGrabbed, BurningRubber,\n"
"#   PirateInPeril, TroubleInLumpySpace\n"
"# This only flips the unlock flag; it does not fill in map completion.\n"
"#adventure.AppleThief = true\n"
"#adventure.WinterIsComing = true\n",
    f);
  fclose(f);
  debugPrintf("[save] wrote %s (every option commented out)\n", path);
}

static int mentions_key(const char *text, const char *key) {
  const size_t kl = strlen(key);
  for (const char *p = text; p && *p; ) {
    while (*p == ' ' || *p == '\t' || *p == '#') p++;
    if (!strncmp(p, key, kl)) {
      const char *q = p + kl;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '=') return 1;
    }
    p = strchr(p, '\n');
    if (p) p++;
  }
  return 0;
}
/* Options added after a player already has a save.txt. Each block is appended
 * once, only if the file does not already mention the key -- so a new setting
 * shows up without anyone deleting their file, and editing a comment back in
 * does not duplicate it. `keys[1]` is an optional alias.
 *
 * Everything currently in the template shipped together, so this is empty. Add
 * a row here, not just to write_template(), when a new setting is introduced:
 * write_template only runs for a file that does not exist yet. */
static const struct { const char *keys[2]; const char *block; } ADDED[] = {
  /* The unlock section, with every tower listed one per line. Keyed on
   * unlock_all_towers, which the block itself mentions, so it is appended
   * exactly once to a save.txt that predates it. */
  { { "unlock_all_towers", NULL },
    SAVE_TXT_UNLOCKS },
};

static void append_missing(const char *path) {
  /* save.txt is ~26 KB now that every unlockable name is listed. This buffer
   * only has to be big enough for mentions_key() to SEE a key that is already
   * in the file -- if a key sits past the end, the block is appended again on
   * every launch and the file grows without bound. 16 KB stopped being enough
   * the moment the listing was added. */
  static char text[128 * 1024];
  FILE *f = fopen(path, "r");
  if (!f) return;
  const size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = 0;
  for (size_t i = 0; i < sizeof ADDED / sizeof *ADDED; i++) {
    if (!ADDED[i].keys[0] || !ADDED[i].block) continue;      /* placeholder row */
    if (mentions_key(text, ADDED[i].keys[0]) || mentions_key(text, ADDED[i].keys[1])) continue;
    if (!(f = fopen(path, "a"))) return;
    fprintf(f, "%s\n%s", (n && text[n - 1] != '\n') ? "\n" : "", ADDED[i].block);
    fclose(f);
    debugPrintf("[save] added the new \"%s\" option to %s\n", ADDED[i].keys[0], path);
  }
}

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int parse_count(const char *key, const char *v, long long *out) {
  char *end;
  long long x = strtoll(v, &end, 10);
  if (end == v || *end) { debugPrintf("[save] %s: \"%s\" is not a number -- ignored\n", key, v); return 0; }
  if (x < 0) x = 0;
  if (x > 2147483647LL) x = 2147483647LL;               /* the fields are C# int */
  *out = x;
  return 1;
}
static int parse_bool(const char *key, const char *v, int *out) {
  if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1"))  { *out = 1; return 1; }
  if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0")) { *out = 0; return 1; }
  debugPrintf("[save] %s: \"%s\" is not on/off -- ignored\n", key, v);
  return 0;
}

/* WHAT LIVES WHERE IN THE SAVE, established from a real Profile.Save:
 *
 *   inventory.towers  Dictionary<string, TowerMetaData>  -- the 18 CHARACTERS
 *   inventory.items   List<Item>{name,uniqueId,isNew}    -- allies, weapons,
 *                                                           trinkets
 *
 * Allies are NOT towers. A profile that had never seen the collection still
 * listed BusinessMen and BoomerangMonkey under items, and both are allies. An
 * earlier version of this file put all 82 characters AND allies into
 * inventory.towers and the game crashed; that is why the split matters.
 *
 * Ids come from the Blooncyclopedia lists, converted to the save's naming
 * (title-case words, punctuation dropped) and then CHECKED against the strings
 * in the game's own bundles -- all 348 resolve. Six characters and three items
 * differ from their display names:
 *   Princess Bubblegum -> Bubblegum       Dungeon Finn -> WarriorFinn
 *   Tuxedo Jake -> SerenadingJake         Warrior Bubblegum -> WarmasterBubblegum
 *   Hunter Marceline -> MarcelineTheVampireHunter
 *   Monkey Farmer -> BananaFarmer         COBRA -> Cobra
 *   King of Ooo's Scepter -> KingOfOoosSceptre                              */
static const char *ALL_TOWERS[] = {
  "Finn", "Jake", "Max", "Bubblegum",
  "IceKing", "CaptainCassie", "Marceline", "Sam",
  "FlamePrincess", "C4Charlie", "Sai", "SuperMonkey",
  "WarriorFinn", "SerenadingJake", "JuggernautMax", "WarmasterBubblegum",
  "CommanderCassie", "MarcelineTheVampireHunter",
};
static const char *ALL_ITEMS[] = {
  "Abracadaniel", "AncientPsychicTandemWarElephant", "BananaAirCorps",
  "BananaGuards", "BananaMan", "BettyGrof",
  "Billy", "Bmo", "BoomerangMonkey",
  "BusinessMen", "CinnamonBun", "Clarence",
  "Cobra", "DartMonkey", "DirtBeerGuy",
  "DrMonkey", "ElfMonkey", "FlameKing",
  "GhostPrincess", "GrassyWizard", "GrobGobGlobGrod",
  "Gumbald", "Gunter", "HolidayBmo",
  "HunsonAbadeer", "HuntressWizard", "IceMonkey",
  "KingOfOoo", "LadyRainicorn", "LaserButterfly",
  "Lemonhope", "LumpySpacePrincess", "Maja",
  "MartianTransport", "Martin", "Minipults",
  "Moe", "MonkeyApprentice", "BananaFarmer",
  "MusclePrincess", "Neptr", "PartyGod",
  "PeppermintButler", "PirateCrew", "Rattleballs",
  "Ricardio", "Scorcher", "Shoko",
  "SlimePrincess", "SniperMonkey", "SpaceLards",
  "Squadron", "StarMan", "Starchy",
  "SuperFans", "SusanStrong", "TechnologicalTerror",
  "TinyManticore", "TrainBoss", "Treetrunks",
  "VampireKing", "WaterNymph", "WildberryPrincess",
  "WizardLord", "4DSword", "AbracadanielsWand",
  "AcousticGuitar", "AxBass", "BallBlamBurglerber",
  "Bananarangs", "Banjo", "BarbedDarts",
  "BlessedDart", "Bloonsbane", "BomMo",
  "BombChain", "Bomba", "BreadstickWand",
  "ButterscotchBomb", "CandyBomb", "CandyCaneShotgun",
  "CandyDuckAxe", "CandyHorseTranquilizer", "CandyMicrophone",
  "CaptainTreeTrunksCutlass", "CarbBomb", "CherryBlossomWand",
  "CherryBomb", "CryoBomb", "CyberneticArrow",
  "DemonBloodSword", "DevilMonsterBass", "DrMonkeysSecretWeapon",
  "DragonFangs", "DrillerDarts", "DynamiteStack",
  "ElectrodeGun", "Excandybur", "ExplodingPineapples",
  "FinnSword", "FinnsFlute", "FireGuitar",
  "FireKingdomScepter", "FireSword", "FrozenThrowingKnives",
  "Fumigator", "GarlicBomb", "GlobsSword",
  "GoldenViola", "GrassSword", "GrassWand",
  "GrenadeOfGlob", "GuntersTaser", "GwensFlamethrower",
  "HonestyBells", "JakesSword", "JakesViola",
  "JewelledCabasas", "JingleBomb", "LightningBolts",
  "Liquifier", "LiquidPyrotechnicsLauncher", "LunaticBass",
  "MartianBlaster", "MartianMic", "MilitaryDarts",
  "Moap", "MouthOrgan", "MushroomBomb",
  "NailGun", "NightSword", "Nothung",
  "PenguinShell", "PeppermintBattleAxe", "PhoenixWand",
  "PressureHose", "RainbowGlitterWand", "RazorBats",
  "RepairedViola", "RevengeStick", "RodOfNiceness",
  "RootSword", "SassageFlare", "Scarlet",
  "SilverShurikens", "SilverTippedStakes", "SnakeDarts",
  "SniperRifle", "SoulRedeemerSword", "SpiderWand",
  "SplodeyDarts", "StickyShots", "TheLover",
  "ThievesKatana", "ThoughtCannonWand", "Thundersword",
  "TimeBomb", "UnimaginablyAmazingSword", "WandOfDispersement",
  "WebGun", "WishyWand", "WizardLordWand",
  "WizardThiefWand", "XergioksWand", "AbracadanielsHeadband",
  "AbrahamLincolnsPenny", "AntiCamoDust", "AntiGravityToteChamber",
  "Apple", "ApprenticeCap", "ArrowOfIce",
  "BabyBlanket", "BabyTooth", "BagOfLollies",
  "BakersShard", "BananaReplicator", "Basketball",
  "BeANinja", "BeauteousWings", "BigRedButton",
  "BlackBowTie", "BlazingFeet", "BloonTrap",
  "BmosSkateboard", "BoobooSousa", "BottleRocket",
  "BoxOfDirt", "BrainFood", "BranchesOfPalm",
  "BubblegumsHair", "CandyDiveSuit", "CandySeeds",
  "CandycornSpear", "CaptainTreeTrunksEyepatch", "CarlTheGem",
  "CheesyDog", "ClaBlade", "Condiments",
  "CosmicGauntlets", "Cryojet", "CrystalGemApple",
  "CrystalMergenceOfDestruction", "CursedIceRing", "DaggerOfChilledGlass",
  "DarkTempleIdol", "DaveyStache", "DeathsDrums",
  "DemonHeart", "DemonicWishingEye", "DiveSuit",
  "DoomGauntlets", "DragonEyes", "ElderPlopsScepter",
  "ElementalStaff", "EnchantedBoomerang", "EngineersBlueprints",
  "EyeFlail", "FinnsCrossbow", "FireCrown",
  "FlowerCrown", "FreezingPotionA", "FutureCrystal",
  "GauntletOfBones", "GauntletOfTheHero", "GemmaTheGemstone",
  "GentleLasers", "GiantTranq", "GlaiveOfTheAncients",
  "GlassesOfNerdicon", "GlobsHelmet", "Googoomamameter",
  "GrapplingCrossbow", "GraveRing", "Hambo",
  "HeartGauntlets", "HollyJollyScarf", "HollyJollySweater",
  "HorseySoap", "IceBull", "IceCreamSundae",
  "IceCrook", "IcebergBlade", "IronHull",
  "IssueOfBle", "JamesLuckyCoin", "Jetpack",
  "KingOfOoosSceptre", "KnifeStormCloud", "LambRelic",
  "Lemonsweets", "LittleDude", "LumpinDeliciousSandwiches",
  "LuteSuit", "MaceStake", "MagicCarpet",
  "MagicCoinPurse", "MagicDoorPortal", "MagicMansHat",
  "MagicPowder", "MagicSpanner", "MargaretsMusicBox",
  "MartianTrackingDevice", "MaskOfShadows", "MedallionOfBrogends",
  "MindGames", "Missile", "MonkeyAcademyDegree",
  "MonkeyKingsRobe", "MortarHelmet", "MysteryCavePick",
  "NightVisionXRayGoggles", "NinjaDagger", "NinjaHeadband",
  "NumbChuks", "PaperPlateMask", "PeacockHat",
  "PennysDagger", "PinkSweater", "PirateHat",
  "PlasmaGoggles", "PocketWatch", "PowerRingOfChill",
  "PowerRingOfDamage", "PowerRingOfPierce", "PowerRingOfPoison",
  "PowerRingOfRange", "PowerRingOfSpeed", "PowerRingOfStrength",
  "Powerometer", "PrincessPlant", "ProtectionGem",
  "RCPlane", "Rainicornicopia", "RedBowTie",
  "RedCowboyBoots", "RenceHilt", "RoboMonkeyVisor",
  "RockShirt", "RoyalMedalForHeroicBravery", "SacredSpringScarf",
  "ShardOfEverfrost", "SilverDagger", "SirenHat",
  "SniperBeret", "SoftPretzels", "SoulStone",
  "SpecialSentientSandwich", "SpoonOfProsperity", "StoneOfAncientKnowledge",
  "StoneSkinPotion", "Strawberry", "StrikerJonesCap",
  "SuperPorp", "Taser", "ThiefCrossbow",
  "ThiefKingsDagger", "TigerClaw", "TikiShield",
  "TimeTravelMachine", "TreasureChestKeys", "TreetrunksAppleKnife",
  "TreetrunksDagger", "Trident", "UniversalCoin",
  "VorpalHand", "WarningHorn", "WarpaintMud",
  "WhistlingBook", "WindmillDagger", "WizardNunchuks",
};
#define N_ALL_ITEMS ((int)(sizeof ALL_ITEMS / sizeof *ALL_ITEMS))
#define N_ALL_TOWERS ((int)(sizeof ALL_TOWERS / sizeof *ALL_TOWERS))

/* towerMaxLevel is 10 in the game's own constants -- the old template here
 * suggested "tower.Finn = 20", which is over the cap. */
#define TOWER_LEVEL_MAX  10
/* An arbitrary but deliberate ceiling on copies of one item. There is no
 * quantity field: N copies means N more entries in inventory.items, so
 * "max" has to mean something finite. 10 covers equipping the same trinket
 * to every character at once and keeps the save a sane size. */
#define ITEM_COPIES_MAX  10

static int dyn_add(Dyn *tab, int *n, const char *name, const char *val,
                   const char *what, int as_bool) {
  if (*n >= N_DYN) { debugPrintf("[save] too many %s entries -- \"%s\" ignored\n", what, name); return 0; }
  long long v = -1;
  if (as_bool) { int b = -1; if (!parse_bool(name, val, &b)) return 0; v = b; }
  else if (!parse_count(name, val, &v)) return 0;
  snprintf(tab[*n].name, sizeof tab[*n].name, "%s", name);
  tab[*n].v = v;
  (*n)++;
  return 1;
}

static int read_config(void) {
  char path[640], line[256];
  for (int i = 0; i < N_FIELDS; i++) { g_val[i] = -1; g_str[i][0] = 0; }
  g_ntower = g_nadv = g_nunlock = 0;
  g_unlock_all = g_unlock_items = -1;
  g_item_count = 1; g_tower_level = -1;
  snprintf(path, sizeof path, "%s/save.txt", bp_game_root());
  FILE *f = fopen(path, "r");
  if (!f) { write_template(path); return 0; }
  fclose(f);
  append_missing(path);
  if (!(f = fopen(path, "r"))) return 0;
  while (fgets(line, sizeof line, f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;                                  /* '#' starts a comment anywhere */
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;

    if (!strcmp(key, "unlock_all_towers")) {
      int b = -1;
      if (parse_bool(key, val, &b)) { g_unlock_all = b; g_any |= (b > 0); }
      continue;
    }
    if (!strcmp(key, "item_count")) {
      long long v = -1;
      if (parse_count(key, val, &v)) {
        if (v < 1) v = 1;
        if (v > ITEM_COPIES_MAX) {
          debugPrintf("[save] item_count %lld is more than the %d this editor will add "
                      "-- using %d\n", v, ITEM_COPIES_MAX, ITEM_COPIES_MAX);
          v = ITEM_COPIES_MAX;
        }
        g_item_count = v; g_any = 1;
      }
      continue;
    }
    if (!strcmp(key, "tower_level")) {
      long long v = -1;
      if (parse_count(key, val, &v)) {
        if (v > TOWER_LEVEL_MAX) {
          debugPrintf("[save] tower_level %lld is above the game's cap of %d -- using %d\n",
                      v, TOWER_LEVEL_MAX, TOWER_LEVEL_MAX);
          v = TOWER_LEVEL_MAX;
        }
        if (v >= 1) { g_tower_level = v; g_any = 1; }
      }
      continue;
    }
    if (!strcmp(key, "max_everything")) {
      int b = -1;
      if (parse_bool(key, val, &b) && b > 0) {
        g_unlock_all = g_unlock_items = 1;
        g_item_count = ITEM_COPIES_MAX;
        g_tower_level = TOWER_LEVEL_MAX;
        g_any = 1;
      }
      continue;
    }
    if (!strcmp(key, "unlock_all_items")) {
      int b = -1;
      if (parse_bool(key, val, &b)) { g_unlock_items = b; g_any |= (b > 0); }
      continue;
    }
    if (!strncmp(key, "unlock.", 7)) {
      g_any |= dyn_add(g_unlock, &g_nunlock, key + 7, val, "unlock", 1);
      continue;
    }
    if (!strncmp(key, "tower.", 6)) {
      g_any |= dyn_add(g_tower, &g_ntower, key + 6, val, "tower", 0);
      continue;
    }
    if (!strncmp(key, "adventure.", 10)) {
      g_any |= dyn_add(g_adv, &g_nadv, key + 10, val, "adventure", 1);
      continue;
    }
    int i;
    for (i = 0; i < N_FIELDS; i++) if (!strcmp(key, FIELDS[i].key)) break;
    if (i == N_FIELDS) { debugPrintf("[save] unknown setting \"%s\" -- ignored\n", key); continue; }
    if (FIELDS[i].kind == F_STR) {
      snprintf(g_str[i], sizeof g_str[i], "%s", val);
      g_any = 1;
    } else if (FIELDS[i].kind == F_BOOL) {
      int b = -1;
      if (parse_bool(key, val, &b)) { g_val[i] = b; g_any = 1; }
    } else {
      g_any |= parse_count(key, val, &g_val[i]);
    }
  }
  fclose(f);
  return g_any;
}

/* ------------------------------------------------------------------ */
/* crypto: PBKDF2-HMAC-SHA1, AES-128-CBC (libnx)                       */
/* ------------------------------------------------------------------ */
/* Recovered on hardware by source/battd_save_probe.c, which logs what
 * PasswordGenerator.GetPassword actually returns. It is a LITERAL, not derived
 * from the app ID -- which is why a static search over millions of candidates
 * never found it. Version 3 is what this build writes; the others are unknown
 * and refused rather than guessed, so an old save is left alone instead of
 * being mangled. */
static const char *password_for(unsigned long long version) {
  return version == 3 ? "$LXvKp90n$Cc0GTX2nY5" : NULL;
}
static void pbkdf2_sha1(const char *pw, const u8 *salt, size_t slen, int iters, u8 *out, size_t outlen) {
  u8 in[64], u[20], v[20], t[20];
  const size_t pl = strlen(pw);
  u32 block = 1;
  for (size_t done = 0; done < outlen; block++) {
    memcpy(in, salt, slen);
    in[slen] = (u8)(block >> 24); in[slen + 1] = (u8)(block >> 16);
    in[slen + 2] = (u8)(block >> 8); in[slen + 3] = (u8)block;
    hmacSha1CalculateMac(u, pw, pl, in, slen + 4);
    memcpy(t, u, 20);
    for (int j = 1; j < iters; j++) {
      hmacSha1CalculateMac(v, pw, pl, u, 20);
      memcpy(u, v, 20);
      for (int k = 0; k < 20; k++) t[k] ^= u[k];
    }
    const size_t n = outlen - done < 20 ? outlen - done : 20;
    memcpy(out + done, t, n);
    done += n;
  }
}
static void derive(unsigned long long version, const u8 salt[24], u8 key[16], u8 iv[16]) {
  u8 dk[32];
  pbkdf2_sha1(password_for(version), salt, 24, 10, dk, 32);
  memcpy(iv, dk, 16);                                     /* first GetBytes(16) -> IV  */
  memcpy(key, dk + 16, 16);                               /* second GetBytes(16) -> Key */
}

/* ------------------------------------------------------------------ */
/* container                                                           */
/* ------------------------------------------------------------------ */
/* Decode a Profile.Save. On success *hdr_len covers version+length+record,
 * *json is malloc'd (NUL-terminated, BOM kept) and 1 is returned. */
static int save_decode(const u8 *b, size_t n, size_t *hdr_len, unsigned long long *pwver,
                       char **json, size_t *jlen, const char **why) {
  int32_t ver, rlen;
  if (n < 8) { *why = "too short"; return 0; }
  memcpy(&ver, b, 4); memcpy(&rlen, b + 4, 4);
  if (ver != 1 || rlen < 0 || rlen > 256 || (size_t)(8 + rlen + 32) > n) { *why = "unexpected header"; return 0; }
  const size_t h = 8 + (size_t)rlen;
  unsigned long long pv;
  memcpy(&pv, b + h, 8);
  if (!password_for(pv)) { *why = "unknown password version"; return 0; }
  const u8 *salt = b + h + 8, *ct = b + h + 32;
  const size_t clen = n - h - 32;
  if (!clen || clen % 16) { *why = "ciphertext is not whole AES blocks"; return 0; }
  u8 key[16], iv[16];
  derive(pv, salt, key, iv);
  u8 *pt = malloc(clen);
  if (!pt) { *why = "out of memory"; return 0; }
  Aes128CbcContext ctx;
  aes128CbcContextCreate(&ctx, key, iv, false);
  aes128CbcDecrypt(&ctx, pt, ct, clen);
  const u8 pad = pt[clen - 1];
  int ok = pad >= 1 && pad <= 16;
  for (int i = 1; ok && i <= pad; i++) ok = pt[clen - i] == pad;
  if (!ok) { free(pt); *why = "bad padding (wrong key?)"; return 0; }
  z_stream zs; memset(&zs, 0, sizeof zs);
  if (inflateInit(&zs) != Z_OK) { free(pt); *why = "zlib init"; return 0; }
  size_t cap = 1u << 16, len = 0;
  char *out = malloc(cap + 1);
  zs.next_in = pt; zs.avail_in = (uInt)(clen - pad);
  int zr = Z_OK;
  while (out && zr == Z_OK) {
    if (len == cap) {
      if (cap >= MAX_SAVE * 8u) break;
      cap *= 2;
      char *nb = realloc(out, cap + 1);
      if (!nb) { free(out); out = NULL; break; }
      out = nb;
    }
    zs.next_out = (Bytef *)out + len; zs.avail_out = (uInt)(cap - len);
    zr = inflate(&zs, Z_NO_FLUSH);
    len = cap - zs.avail_out;
  }
  inflateEnd(&zs);
  free(pt);
  if (!out || zr != Z_STREAM_END) { free(out); *why = "zlib stream"; return 0; }
  out[len] = 0;
  *hdr_len = h; *pwver = pv; *json = out; *jlen = len;
  return 1;
}

static int save_encode(const u8 *hdr, size_t h, unsigned long long pv, const char *json, size_t jlen,
                       u8 **blob, size_t *blen) {
  uLongf zcap = compressBound((uLong)jlen);
  u8 *z = malloc(zcap + 16);
  if (!z || compress2(z, &zcap, (const Bytef *)json, (uLong)jlen, Z_DEFAULT_COMPRESSION) != Z_OK) { free(z); return 0; }
  const size_t pad = 16 - (zcap % 16);
  memset(z + zcap, (int)pad, pad);
  const size_t clen = zcap + pad;
  const size_t n = h + 32 + clen;
  u8 *b = malloc(n);
  if (!b) { free(z); return 0; }
  memcpy(b, hdr, h);
  if (h == 44) {                                          /* FileFormatV1: what a real save does */
    int32_t count; memcpy(&count, b + 8, 4); count++; memcpy(b + 8, &count, 4);
    const unsigned long long ticks = (unsigned long long)time(NULL) * 10000000ULL + 621355968000000000ULL;
    const unsigned long long modified = ticks | (1ULL << 62);          /* DateTimeKind.Utc */
    memcpy(b + 36, &modified, 8);
  }
  memcpy(b + h, &pv, 8);
  u8 *salt = b + h + 8, key[16], iv[16];
  randomGet(salt, 24);
  derive(pv, salt, key, iv);
  Aes128CbcContext ctx;
  aes128CbcContextCreate(&ctx, key, iv, true);
  aes128CbcEncrypt(&ctx, b + h + 32, z, clen);
  free(z);
  *blob = b; *blen = n;
  return 1;
}

/* ------------------------------------------------------------------ */
/* JSON: top-level keys only                                           */
/* ------------------------------------------------------------------ */
static size_t skip_string(const char *j, size_t n, size_t p) {     /* p at the opening quote */
  for (p++; p < n; p++) {
    if (j[p] == '\\') { p++; continue; }
    if (j[p] == '"') return p + 1;
  }
  return n;
}
static size_t skip_value(const char *j, size_t n, size_t p) {
  if (p >= n) return n;
  if (j[p] == '"') return skip_string(j, n, p);
  if (j[p] == '{' || j[p] == '[') {
    int depth = 0;
    for (; p < n; p++) {
      if (j[p] == '"') { p = skip_string(j, n, p) - 1; continue; }
      if (j[p] == '{' || j[p] == '[') depth++;
      else if ((j[p] == '}' || j[p] == ']') && --depth == 0) return p + 1;
    }
    return n;
  }
  while (p < n && j[p] != ',' && j[p] != '}' && j[p] != ']' && !isspace((unsigned char)j[p])) p++;
  return p;
}
/* Value extent of `key` inside the object whose '{' is at j[os]. */
static int obj_value(const char *j, size_t n, size_t os, const char *key,
                     size_t *vs, size_t *ve) {
  if (os >= n || j[os] != '{') return 0;
  const size_t kl = strlen(key);
  size_t p = os + 1;
  for (;;) {
    while (p < n && (isspace((unsigned char)j[p]) || j[p] == ',')) p++;
    if (p >= n || j[p] == '}') return 0;
    if (j[p] != '"') return 0;
    const size_t ks = p + 1, ke = skip_string(j, n, p) - 1;
    p = ke + 1;
    while (p < n && isspace((unsigned char)j[p])) p++;
    if (p >= n || j[p] != ':') return 0;
    p++;
    while (p < n && isspace((unsigned char)j[p])) p++;
    const size_t v0 = p, v1 = skip_value(j, n, p);
    if (ke - ks == kl && !memcmp(j + ks, key, kl)) { *vs = v0; *ve = v1; return 1; }
    p = v1;
  }
}

/* Dotted path: "resources.money", "inventory.towers.Finn.level". Descends one
 * object per segment; returns 0 the moment a segment is absent, so a save from
 * a different game version is skipped rather than half-edited. */
static int path_value(const char *j, size_t n, const char *path,
                      size_t *vs, size_t *ve) {
  size_t os = 0;
  while (os < n && j[os] != '{') os++;                     /* skip the BOM */
  if (os >= n) return 0;
  char seg[64];
  const char *p = path;
  for (;;) {
    const char *dot = strchr(p, '.');
    const size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (l == 0 || l >= sizeof seg) return 0;
    memcpy(seg, p, l); seg[l] = 0;
    size_t s, e;
    if (!obj_value(j, n, os, seg, &s, &e)) return 0;
    if (!dot) { *vs = s; *ve = e; return 1; }
    os = s;                                                /* descend */
    p = dot + 1;
  }
}
static int splice(char **j, size_t *n, size_t s, size_t e, const char *rep) {
  const size_t rl = strlen(rep), nn = *n - (e - s) + rl;
  char *b = malloc(nn + 1);
  if (!b) return 0;
  memcpy(b, *j, s); memcpy(b + s, rep, rl); memcpy(b + s + rl, *j + e, *n - e);
  b[nn] = 0;
  free(*j); *j = b; *n = nn;
  return 1;
}

static char g_changes[1024];
static void note(const char *fmt, const char *field, const char *from, const char *to) {
  size_t l = strlen(g_changes);
  snprintf(g_changes + l, sizeof g_changes - l, fmt, l ? ", " : "", field, from, to);
}
/* REFUSE TO OVERWRITE A STRUCTURED VALUE.
 *
 * path_value() returns whatever span the key maps to, and for an object or an
 * array that is the whole "{...}" / "[...]". Splicing a scalar over it produces
 * JSON the game cannot deserialise back into the field's type.
 *
 * This is not hypothetical for the tower levels: ProfileModel.Inventory.
 * TowerMetaData.level is a CryptVarInt32, whose fields are a byte[] val, two
 * int key indices and a byte[] data. If Unity serialises that as an object,
 * "tower.Finn = 20" would replace the object with 20 and break the save. The
 * editor keeps a .orig copy, but a refusal that says so is better than a
 * restore. If it turns out to be a plain integer, nothing here changes. */
static int scalar_span(const char *j, size_t s, size_t e, const char *path) {
  while (s < e && (j[s] == ' ' || j[s] == '\t' || j[s] == '\n' || j[s] == '\r')) s++;
  if (s < e && (j[s] == '{' || j[s] == '[')) {
    debugPrintf("[save] \"%s\" is %s in this save, not a plain value -- left alone "
                "(editing it would corrupt the field)\n",
                path, j[s] == '{' ? "an object" : "an array");
    return 0;
  }
  return 1;
}
/* UNLOCK A TOWER: add a key to inventory.towers.
 *
 * This is the one place the editor INSERTS rather than replaces, so it is
 * deliberately narrow. Ownership in this game is exactly "is there a key for
 * it in inventory.towers" -- a real profile has five there, while seenTowers
 * holds two names that are NOT owned, so seenTowers is a UI badge and not the
 * gate. The value copies the shape the save already uses for an unequipped
 * tower: {"level":1,"equippedItems":[]}.
 *
 * Refuses anything that is not a plain identifier, so a stray character in
 * save.txt cannot inject JSON. Returns 1 if it added one. */
static int tower_exists(const char *j, size_t n, const char *name) {
  char p[128]; size_t s, e;
  snprintf(p, sizeof p, "inventory.towers.%s", name);
  return path_value(j, n, p, &s, &e);
}
static int unlock_tower(char **j, size_t *n, const char *name) {
  for (const char *p = name; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_') {
      debugPrintf("[save] \"%s\" is not a valid tower name -- skipped\n", name);
      return 0;
    }
  if (!*name) return 0;
  if (tower_exists(*j, *n, name)) return 0;                  /* already owned */
  size_t s, e;
  if (!path_value(*j, *n, "inventory.towers", &s, &e)) {
    debugPrintf("[save] this save has no inventory.towers -- cannot unlock\n");
    return 0;
  }
  if ((*j)[s] != '{') return 0;
  /* after the '{': an empty object takes no comma, a populated one does */
  size_t at = s + 1;
  while (at < e && ((*j)[at]==' '||(*j)[at]=='\t'||(*j)[at]=='\n'||(*j)[at]=='\r')) at++;
  const int empty = (at < e && (*j)[at] == '}');
  char ins[160];
  snprintf(ins, sizeof ins, "\"%s\":{\"level\":1,\"equippedItems\":[]}%s",
           name, empty ? "" : ",");
  if (!splice(j, n, s + 1, s + 1, ins)) return 0;
  return 1;
}
/* UNLOCK AN ITEM: append to inventory.items.
 *
 * Different shape from a tower. items is a LIST of
 * {"name":...,"uniqueId":N,"isNew":true} and every entry needs an id no other
 * entry uses; itemUniqueIdSupplier holds the next one. So: read the supplier,
 * append entries counting up from it, write the supplier back. Names are added
 * to seenItems too, otherwise the whole collection screen is a wall of "new"
 * badges.
 *
 * Skips anything already present, so running twice adds nothing and an item
 * the player earned keeps its original id. */
static int item_present(const char *j, size_t n, const char *name) {
  /* "name":"<name>" inside the items array; a plain search is enough because
   * ids are alphanumeric and cannot appear as a substring of a longer id
   * without the closing quote differing. */
  char pat[96];
  snprintf(pat, sizeof pat, "\"name\":\"%s\"", name);
  const size_t pl = strlen(pat);
  for (size_t i = 0; i + pl <= n; i++) if (!memcmp(j + i, pat, pl)) return 1;
  /* the encoder may emit a space after the colon */
  snprintf(pat, sizeof pat, "\"name\": \"%s\"", name);
  const size_t pl2 = strlen(pat);
  for (size_t i = 0; i + pl2 <= n; i++) if (!memcmp(j + i, pat, pl2)) return 1;
  return 0;
}
static int valid_id(const char *s) {
  if (!*s) return 0;
  for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') return 0;
  return 1;
}
/* copies: how many of each. The save has no quantity field -- an item you own
 * twice is simply two entries with the same name and different uniqueIds, which
 * is how the game lets you equip the same trinket to two characters. So N
 * copies means N entries. Existing copies are counted, and only the shortfall
 * is added, so raising item_count from 2 to 5 tops up rather than duplicating. */
static int item_count_in(const char *j, size_t n, const char *name) {
  char pat[96], pat2[96];
  snprintf(pat,  sizeof pat,  "\"name\":\"%s\"",  name);
  snprintf(pat2, sizeof pat2, "\"name\": \"%s\"", name);
  const size_t a = strlen(pat), b = strlen(pat2);
  int c = 0;
  for (size_t i = 0; i + a <= n; i++) if (!memcmp(j + i, pat, a)) c++;
  for (size_t i = 0; i + b <= n; i++) if (!memcmp(j + i, pat2, b)) c++;
  return c;
}
static int unlock_items(char **j, size_t *n, const char *const *names, int count, int copies) {
  size_t s, e;
  if (!path_value(*j, *n, "inventory.itemUniqueIdSupplier", &s, &e)) {
    debugPrintf("[save] no inventory.itemUniqueIdSupplier -- cannot add items\n");
    return 0;
  }
  char num[32];
  snprintf(num, sizeof num, "%.*s", (int)(e - s) < 31 ? (int)(e - s) : 31, *j + s);
  long long next = atoll(num);
  if (next <= 0) next = 1;

  int added = 0;
  for (int k = 0; k < count; k++) {
    const char *nm = names[k];
    if (!valid_id(nm)) { debugPrintf("[save] \"%s\" is not a valid item name -- skipped\n", nm); continue; }
    int want = copies - item_count_in(*j, *n, nm);
    for (; want > 0; want--) {
    size_t as, ae;
    if (!path_value(*j, *n, "inventory.items", &as, &ae) || (*j)[as] != '[') { k = count; break; }
    size_t at = as + 1;
    while (at < ae && ((*j)[at]==' '||(*j)[at]=='\t'||(*j)[at]=='\n'||(*j)[at]=='\r')) at++;
    const int empty = (at < ae && (*j)[at] == ']');
    char ins[192];
    snprintf(ins, sizeof ins, "{\"name\":\"%s\",\"uniqueId\":%lld,\"isNew\":false}%s",
             nm, next, empty ? "" : ",");
    if (!splice(j, n, as + 1, as + 1, ins)) { k = count; break; }
    /* and mark it seen, so the collection is not all "new" */
    if (path_value(*j, *n, "inventory.seenItems", &as, &ae) && (*j)[as] == '[') {
      at = as + 1;
      while (at < ae && ((*j)[at]==' '||(*j)[at]=='\t'||(*j)[at]=='\n'||(*j)[at]=='\r')) at++;
      const int sempty = (at < ae && (*j)[at] == ']');
      char sins[96];
      snprintf(sins, sizeof sins, "\"%s\"%s", nm, sempty ? "" : ",");
      splice(j, n, as + 1, as + 1, sins);
    }
    next++; added++;
    }
  }
  if (added) {
    /* write the supplier back, from scratch: the spans above have moved */
    if (path_value(*j, *n, "inventory.itemUniqueIdSupplier", &s, &e)) {
      char rep[32];
      snprintf(rep, sizeof rep, "%lld", next);
      splice(j, n, s, e, rep);
    }
  }
  return added;
}
/* ROUTE A NAME TO THE RIGHT HALF OF THE SAVE.
 *
 * unlock.<Name> used to call unlock_tower() whatever the name was, so
 * "unlock.Abracadaniel = true" would have put an ALLY into inventory.towers --
 * precisely the mistake that crashed the game when the whole list went in
 * there. A name is a character or an item or neither, and the tables say
 * which; anything not in either is refused rather than guessed at. */
static int in_table(const char *const *tab, int n, const char *name) {
  for (int i = 0; i < n; i++) if (!strcmp(tab[i], name)) return 1;
  return 0;
}
static int unlock_named(char **j, size_t *n, const char *name, int *towers, int *items) {
  if (in_table(ALL_TOWERS, N_ALL_TOWERS, name)) { *towers += unlock_tower(j, n, name); return 1; }
  if (in_table(ALL_ITEMS,  N_ALL_ITEMS,  name)) {
    const char *one[1] = { name };
    *items += unlock_items(j, n, one, 1, (int)g_item_count);
    return 1;
  }
  debugPrintf("[save] \"%s\" is not a character or an item in this game -- skipped. "
              "See the lists in save.txt; names are the internal ids, not the "
              "display names (Princess Bubblegum is \"Bubblegum\").\n", name);
  return 0;
}
static void set_int(char **j, size_t *n, const char *path, long long v) {
  size_t s, e;
  if (v < 0) return;
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  if (!scalar_span(*j, s, e, path)) return;
  char old[48], rep[32];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  snprintf(rep, sizeof rep, "%lld", v);
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}
static void set_bool(char **j, size_t *n, const char *path, int v) {
  size_t s, e;
  if (v < 0) return;
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  if (!scalar_span(*j, s, e, path)) return;
  char old[16];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  const char *rep = v ? "true" : "false";
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}
/* Strings are spliced WITH their quotes, so the replacement is a valid JSON
 * value and not a fragment. Anything needing an escape is refused rather than
 * written half-quoted -- the fields this reaches are enum names. */
static void set_str(char **j, size_t *n, const char *path, const char *v) {
  size_t s, e;
  if (!v || !*v) return;
  if (strpbrk(v, "\"\\")) {
    debugPrintf("[save] %s: value contains a quote or backslash -- refused\n", path);
    return;
  }
  if (!path_value(*j, *n, path, &s, &e)) {
    debugPrintf("[save] the save has no \"%s\" -- left alone\n", path); return;
  }
  if (!scalar_span(*j, s, e, path)) return;
  char old[80], rep[80];
  snprintf(old, sizeof old, "%.*s", (int)(e - s), *j + s);
  snprintf(rep, sizeof rep, "\"%s\"", v);
  if (strcmp(old, rep) && splice(j, n, s, e, rep)) note("%s%s %s->%s", path, old, rep);
}

static u8 *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  u8 *b = (sz > 0 && (unsigned long)sz <= MAX_SAVE) ? malloc((size_t)sz) : NULL;
  if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
  fclose(f);
  *n = b ? (size_t)sz : 0;
  return b;
}
static int write_file(const char *path, const u8 *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(b, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

static void patch_save(const char *path) {
  size_t n = 0, h = 0, jl = 0;
  unsigned long long pv = 0;
  char *json = NULL;
  const char *why = "";
  u8 *orig = read_file(path, &n);
  if (!orig) { debugPrintf("[save] cannot read %s\n", path); return; }
  if (!save_decode(orig, n, &h, &pv, &json, &jl, &why)) {
    debugPrintf("[save] %s: not decodable (%s) -- left alone\n", path, why);
    free(orig);
    return;
  }
  g_changes[0] = 0;
  for (int i = 0; i < N_FIELDS; i++) {
    switch (FIELDS[i].kind) {
      case F_INT:  set_int(&json, &jl, FIELDS[i].path, g_val[i]);        break;
      case F_BOOL: set_bool(&json, &jl, FIELDS[i].path, (int)g_val[i]);  break;
      case F_STR:  set_str(&json, &jl, FIELDS[i].path, g_str[i]);        break;
    }
  }
  /* Unlocks first, so a tower.<Name> level below can apply to one that was
   * just added. */
  int added = 0, items_added = 0;
  if (g_unlock_all > 0)
    for (int i = 0; i < N_ALL_TOWERS; i++) added += unlock_tower(&json, &jl, ALL_TOWERS[i]);
  if (g_unlock_items > 0)
    items_added += unlock_items(&json, &jl, ALL_ITEMS, N_ALL_ITEMS, (int)g_item_count);
  for (int i = 0; i < g_nunlock; i++)
    if (g_unlock[i].v > 0) unlock_named(&json, &jl, g_unlock[i].name, &added, &items_added);
  if (added || items_added) {
    static char c[48];
    snprintf(c, sizeof c, "%d towers, %d items", added, items_added);
    note("%s%s %s->%s", "unlocked", "0", c);
  }

  /* tower_level applies to every character the save holds, after the unlocks,
   * so it covers ones added a moment ago. An explicit tower.<Name> below still
   * wins, because it is applied after this. */
  if (g_tower_level >= 1)
    for (int i = 0; i < N_ALL_TOWERS; i++) {
      char p[128];
      snprintf(p, sizeof p, "inventory.towers.%s.level", ALL_TOWERS[i]);
      size_t ls, le;
      if (path_value(json, jl, p, &ls, &le)) set_int(&json, &jl, p, g_tower_level);
    }

  for (int i = 0; i < g_ntower; i++) {
    char p[128];
    snprintf(p, sizeof p, "inventory.towers.%s.level", g_tower[i].name);
    set_int(&json, &jl, p, g_tower[i].v);
  }
  for (int i = 0; i < g_nadv; i++) {
    char p[160];
    snprintf(p, sizeof p, "adventureData.adventureProgress.%s.isUnlocked", g_adv[i].name);
    set_bool(&json, &jl, p, (int)g_adv[i].v);
  }
  if (!g_changes[0]) {
    debugPrintf("[save] %s already matches save.txt\n", path);
    free(json); free(orig);
    return;
  }
  u8 *blob = NULL; size_t bl = 0;
  if (!save_encode(orig, h, pv, json, jl, &blob, &bl)) {
    debugPrintf("[save] %s: re-encoding failed -- left alone\n", path);
    free(json); free(orig);
    return;
  }
  size_t h2, jl2; unsigned long long pv2; char *check = NULL;         /* verify before writing */
  if (!save_decode(blob, bl, &h2, &pv2, &check, &jl2, &why) || jl2 != jl || memcmp(check, json, jl)) {
    debugPrintf("[save] %s: verification of the new save FAILED (%s) -- left alone\n", path, why);
    free(check); free(blob); free(json); free(orig);
    return;
  }
  free(check);
  char aux[700];
  struct stat st;
  snprintf(aux, sizeof aux, "%s.orig", path);
  if (stat(aux, &st) != 0) {
    if (!write_file(aux, orig, n)) {
      debugPrintf("[save] could not write the backup %s -- not editing without it\n", aux);
      free(blob); free(json); free(orig);
      return;
    }
    debugPrintf("[save] original kept as %s\n", aux);
  }
  snprintf(aux, sizeof aux, "%s.tmp", path);
  int ok = write_file(aux, blob, bl);
  if (ok) {
    remove(path);
    ok = rename(aux, path) == 0 || write_file(path, blob, bl);
    remove(aux);
  }
  fsdevCommitDevice("sdmc");
  debugPrintf("[save] %s %s: %s\n", path, ok ? "updated" : "WRITE FAILED", g_changes);
  free(blob); free(json); free(orig);
}

static int find_saves(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int found = 0;
  struct dirent *e;
  char p[700];
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < 6 && strcmp(e->d_name, "UnityCache")) found += find_saves(p, depth + 1);
    } else if (!strcmp(e->d_name, "Profile.Save")) {
      patch_save(p);
      found++;
    }
  }
  closedir(d);
  return found;
}

void bp_savetool_run(void) {
  if (!read_config()) return;                             /* nothing uncommented */
  char files[640];
  snprintf(files, sizeof files, "%s/files", bp_game_root());
  if (!find_saves(files, 0))
    debugPrintf("[save] save.txt has settings, but there is no Profile.Save under %s yet "
                "(it appears after the first play session)\n", files);
}
