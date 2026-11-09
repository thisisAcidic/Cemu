import unittest

from frametimes import summarize


class SummarizeTest(unittest.TestCase):
    def rows(self, frametimes_us, shader_us=0):
        t = 0
        out = []
        for i, ft in enumerate(frametimes_us):
            t += ft
            out.append({'frame': i, 'timestamp_us': t, 'frametime_us': ft, 'shadercreate_us': shader_us})
        return out

    def test_steady_60(self):
        s = summarize(self.rows([16666] * 600))
        self.assertEqual(s['frames'], 600)
        self.assertAlmostEqual(s['avg_fps'], 60.0, places=1)
        self.assertEqual(s['pct_within_16_7'], 100.0)
        self.assertEqual(s['frames_over_50ms'], 0)

    def test_one_hitch(self):
        s = summarize(self.rows([16666] * 99 + [50001]))
        self.assertEqual(s['frames_over_50ms'], 1)
        self.assertEqual(s['pct_within_16_7'], 99.0)
        self.assertGreater(s['p99_ms'], 16.7)

    def test_shader_time_sum(self):
        self.assertAlmostEqual(summarize(self.rows([33333] * 10, shader_us=1500))['shadercreate_ms_total'], 15.0)


if __name__ == '__main__':
    unittest.main()
