push_textdomain("tribes")

dirname = path.dirname (__file__)

wl.Descriptions():new_worker_type {
   name = "frisians_seamstress_master",
   -- TRANSLATORS: This is a worker name used in lists of workers
   descname = pgettext ("frisians_worker", "Master Seamstress"),
   animation_directory = dirname,
   icon = dirname .. "menu.png",
   vision_range = 2,

   ware_hotspot = {0, 20},

   spritesheets = {
      walk = {
         fps = 15,
         frames = 10,
         columns = 5,
         rows = 2,
         directional = true,
         hotspot = {10, 23}
      },
      walkload = {
         fps = 15,
         frames = 10,
         columns = 5,
         rows = 2,
         directional = true,
         hotspot = {10, 26}
      },
   },
   animations = {
      idle = {
         hotspot = {9, 23}
      },
   },
}

pop_textdomain()
