"""The online character screen shows each opponent's live pick and names who it waits on."""
import unittest

from squad_wiring_test import GAME, function, read


class OpponentPicksLine(unittest.TestCase):
    def setUp(self):
        self.css = read(GAME / 'shim/mu_slippi_css.c')

    def test_line_is_created_last_and_drawn_only_while_connected(self):
        init = self.css[self.css.index('dt.text = t;'):]
        init = init[:init.index('/* ---- chat')]
        self.assertTrue(init.rstrip().rstrip('}').rstrip().endswith('/* their picks */'),
                        'the picks line must be the last subtext so the fixed indexes stay put')
        think = function(self.css, 'text_think')
        self.assertIn('if (conn == MU_SLP_MM_CONNECTION_SUCCESS) opponent_picks_line(m, picks, sizeof picks);', think)
        self.assertIn('else picks[0] = 0;', think)

    def test_line_wording(self):
        line = function(self.css, 'opponent_picks_line')
        for text in ['"choosing"', '" - locked in"', 'm->names[o->port]', 'o->squad_count > 0', 'if (!o->valid) continue;']:
            self.assertIn(text, line)
        self.assertIn('"Waiting on %s", opp_name[0] ? opp_name : "opponent"', self.css)
        self.assertNotIn('"Waiting on %s", "opponent"', self.css)


    def test_squad_strike_line_has_four_places(self):
        # Squad Strike: three member places and the coin pick, the fourth and last pick, after them.
        place = function(self.css, 'opp_squad_place')
        self.assertIn('if (j == 3) return o->squad_count == 3 && o->char_id != 0xFF ? pick_name(o->char_id) : "-";', place)
        self.assertIn('return j < o->squad_count ? pick_name(o->kinds[j]) : "-";', place)
        self.assertIn('#define OPP_SQUAD_PLACES GMSQUAD_SIZE\n#else\n#define OPP_SQUAD_PLACES 3\n#endif', self.css)
        self.assertIn('for (j = 0; j < OPP_SQUAD_PLACES && used + 1 < size; j++)', function(self.css, 'opponent_picks_line'))


if __name__ == '__main__':
    unittest.main()
