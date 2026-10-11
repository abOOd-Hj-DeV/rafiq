"""Log-mel input contract of Qualcomm's torch_audioset YAMNet export."""
import numpy as np


def yamnet_pcm_features(samples, sample_rate):
    if sample_rate == 48000:
        positions = np.arange(129) - 64
        kernel = np.sinc(positions * (0.95 / 3)) * np.kaiser(129, 8.6)
        kernel = (kernel / kernel.sum()).astype(np.float32)
        samples = np.convolve(samples, kernel, mode="same")[::3]
    elif sample_rate != 16000:
        raise ValueError("YAMNet PCM must be 16kHz or 48kHz")
    return yamnet_features(samples)


def yamnet_features(mono):
    samples = np.asarray(mono, dtype=np.float32)
    if samples.ndim != 1 or len(samples) < 15600 or not np.isfinite(samples).all():
        raise ValueError("YAMNet requires at least 975ms of finite 16kHz mono audio")
    samples = samples[-15600:]
    window = np.zeros(512, dtype=np.float32)
    window[56:456] = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(400) / 400)
    frames = np.lib.stride_tricks.sliding_window_view(
        np.pad(samples, (256, 256), mode="reflect"), 512
    )[::160][:96]
    magnitude = np.abs(np.fft.rfft(frames * window, n=512)).astype(np.float32)
    frequencies = np.linspace(0, 8000, 257)
    edges = np.linspace(2595 * np.log10(1 + 125 / 700),
                        2595 * np.log10(1 + 7500 / 700), 66)
    edges = 700 * (10 ** (edges / 2595) - 1)
    rising = (frequencies[:, None] - edges[:-2]) / (edges[1:-1] - edges[:-2])
    falling = (edges[2:] - frequencies[:, None]) / (edges[2:] - edges[1:-1])
    matrix = np.maximum(0, np.minimum(rising, falling)).astype(np.float32)
    features = np.log(magnitude @ matrix + 0.001)
    return features.reshape(1, 1, 96, 64).astype(np.float32)
