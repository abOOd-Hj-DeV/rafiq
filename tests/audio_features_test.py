import sys
import unittest
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from audio_features import yamnet_features, yamnet_pcm_features


class YamnetFeaturesTests(unittest.TestCase):
    def test_silence_has_exact_shape_dtype_and_log_floor(self):
        result = yamnet_features(np.zeros(15600))
        self.assertEqual(result.shape, (1, 1, 96, 64))
        self.assertEqual(result.dtype, np.float32)
        np.testing.assert_allclose(result, np.log(.001), atol=1e-6)

    def test_sinusoid_is_finite_and_tail_window_is_stable(self):
        t = np.arange(15600) / 16000
        sine = .1 * np.sin(2 * np.pi * 1000 * t)
        result = yamnet_features(sine)
        self.assertTrue(np.isfinite(result).all())
        self.assertGreater(float(result.max()), float(result.min()) + 5)
        np.testing.assert_array_equal(result, yamnet_features(np.concatenate([np.ones(100), sine])))

    def test_rejects_short_nonfinite_or_multichannel_input(self):
        for invalid in (np.zeros(15000), np.zeros((1, 15600)), np.full(15600, np.nan)):
            with self.assertRaises(ValueError):
                yamnet_features(invalid)

    def test_48khz_filter_rejects_aliasing_before_decimation(self):
        t = np.arange(48000) / 48000
        high = .1 * np.sin(2 * np.pi * 12000 * t)
        low = .1 * np.sin(2 * np.pi * 1000 * t)
        self.assertLess(float(yamnet_pcm_features(high, 48000)[:, :, 3:].max()), -5)
        self.assertGreater(float(yamnet_pcm_features(low, 48000).max()), 0)
        with self.assertRaises(ValueError):
            yamnet_pcm_features(low, 44100)


if __name__ == "__main__":
    unittest.main()
