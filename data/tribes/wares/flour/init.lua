push_textdomain("tribes")

local dirname = path.dirname(__file__)

wl.Descriptions():new_ware_type {
   name = "flour",
   -- TRANSLATORS: This is a ware name used in lists of wares
   descname = pgettext("ware", "Flour"),
   icon = dirname .. "menu.png",

   animation_directory = dirname,
   animations = {
      idle = {
         hotspot = { 5, 15 },
      },
   }
}

pop_textdomain()
