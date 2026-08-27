"""
audio_reactive.py
=================
Maps an audio signal to N intensities (one per luminaire), using:

  1. Log/mel filterbank       -> N perceptually-spaced bands
     (leftmost = bass/kick, rightmost = treble)
  2. PER-BAND normalization   -> every luminaire uses the full 0..1 range,
     regardless of bass dominating the raw energy
  3. ATTACK detection         -> half-wave spectral flux, highlights
     onsets/beats instead of just showing the raw "level"
  4. Asymmetric attack/decay envelope -> rises fast, falls smoothly
     (kills flicker without blurring the beat)
  5. Perceptual mapping (gamma) + noise gate

Only depends on numpy + scipy. Loading an mp3 is optional (librosa),
or pass in your own samples (float mono, sample_rate).

Offline use (whole file -> already-computed intensity matrix):
    m = ReactiveMapper(n_bands=8, sr=22050)
    times, I = m.process_offline(samples, sr)   # I.shape == (T, N), in [0,1]
    # I[k] is the vector of N intensities at instant times[k]

Real-time use (streaming, one block at a time):
    m = ReactiveMapper(n_bands=8, sr=22050, hop=512)
    for block in audio_source():        # block of 'hop' mono samples
        I = m.push(block)               # (N,) vector in [0,1] -> send to the luminaires
"""

from dataclasses import dataclass, field
import numpy as np
from scipy.signal import get_window


# ----------------------------------------------------------------------------
# Mel-spaced (perceptual log) triangular filterbank
# ----------------------------------------------------------------------------
def _hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + f / 700.0)


def _mel_to_hz(m):
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def mel_filterbank(n_bands, n_fft, sr, fmin=40.0, fmax=None):
    """(n_bands, n_fft//2+1) matrix of triangular mel-spaced filters.

    fmin=40 Hz puts band 0 right on top of kick/bass. Set fmax to ~sr/2.
    Log spacing matters because music energy distributes logarithmically,
    not linearly -- that's the classic mistake of "just using raw FFT bins".
    """
    if fmax is None:
        fmax = sr / 2.0
    n_freqs = n_fft // 2 + 1
    fft_freqs = np.linspace(0.0, sr / 2.0, n_freqs)

    mel_pts = np.linspace(_hz_to_mel(fmin), _hz_to_mel(fmax), n_bands + 2)
    hz_pts = _mel_to_hz(mel_pts)

    fb = np.zeros((n_bands, n_freqs), dtype=np.float32)
    for b in range(n_bands):
        lo, ctr, hi = hz_pts[b], hz_pts[b + 1], hz_pts[b + 2]
        up = (fft_freqs - lo) / max(ctr - lo, 1e-9)
        dn = (hi - fft_freqs) / max(hi - ctr, 1e-9)
        fb[b] = np.clip(np.minimum(up, dn), 0.0, None)
    # normalize each filter to ~unit area (don't favor wider bands)
    area = fb.sum(axis=1, keepdims=True)
    area[area == 0] = 1.0
    fb /= area
    return fb, hz_pts[1:-1]  # also return the center frequencies


# ----------------------------------------------------------------------------
# Exponential filter with asymmetric attack/decay (envelope follower)
# ----------------------------------------------------------------------------
@dataclass
class ExpEnvelope:
    """y rises with alpha_attack (fast) and falls with alpha_release (slow).

    alpha is in (0,1]; the HIGHER it is, the faster it tracks the input.
    Typical: attack ~0.6 (snaps on the beat), release ~0.08 (smooth fade).
    """
    n: int
    alpha_attack: float = 0.6
    alpha_release: float = 0.08
    y: np.ndarray = field(default=None)

    def __post_init__(self):
        self.y = np.zeros(self.n, dtype=np.float32)

    def update(self, x):
        rising = x > self.y
        a = np.where(rising, self.alpha_attack, self.alpha_release).astype(np.float32)
        self.y += a * (x - self.y)
        return self.y


# ----------------------------------------------------------------------------
# Per-band AGC: normalizes each band against its own recent peak
# ----------------------------------------------------------------------------
@dataclass
class PerBandAGC:
    """peak[b] decays slowly; norm = energy/peak. Makes every band use the
    full 0..1 range on its own -- bass stops dominating the whole show.
    """
    n: int
    decay: float = 0.995     # how fast the "ceiling" falls (per frame)
    floor: float = 1e-4      # avoids dividing by ~0 during silence
    peak: np.ndarray = field(default=None)

    def __post_init__(self):
        self.peak = np.full(self.n, self.floor, dtype=np.float32)

    def update(self, x):
        self.peak = np.maximum(x, self.peak * self.decay)
        self.peak = np.maximum(self.peak, self.floor)
        return np.clip(x / self.peak, 0.0, 1.0)


# ----------------------------------------------------------------------------
# Main mapper
# ----------------------------------------------------------------------------
class ReactiveMapper:
    def __init__(
        self,
        n_bands=8,
        sr=22050,
        n_fft=2048,
        hop=512,
        fmin=40.0,
        fmax=8000.0,         # above ~8 kHz music has little useful energy
        flux_mix=0.6,        # 0=level only, 1=attack/onset only
        gamma=1.6,           # perceptual brightness correction
        gate=0.05,           # below this, the luminaire goes dark (silence)
        attack=0.85,         # snaps on the beat (lower = smoother)
        release=0.10,        # decay fade (lower = longer fade)
        agc_decay=0.995,
    ):
        self.n_bands = n_bands
        self.sr = sr
        self.n_fft = n_fft
        self.hop = hop
        self.flux_mix = flux_mix
        self.gamma = gamma
        self.gate = gate

        self.window = get_window("hann", n_fft, fftbins=True).astype(np.float32)
        self.fb, self.center_hz = mel_filterbank(n_bands, n_fft, sr, fmin, fmax)

        self.agc = PerBandAGC(n_bands, decay=agc_decay)
        self.env = ExpEnvelope(n_bands, attack, release)
        self._prev = np.zeros(n_bands, dtype=np.float32)   # for spectral flux
        self._buf = np.zeros(n_fft, dtype=np.float32)      # streaming buffer

    # ---- core: one audio frame (n_fft samples) -> N intensities ----
    def _frame_to_bands(self, frame):
        spec = np.abs(np.fft.rfft(frame * self.window))
        band_energy = self.fb @ (spec ** 2)          # energy per band
        band_energy = np.sqrt(band_energy)           # back to "amplitude"
        return band_energy.astype(np.float32)

    def _combine(self, band_energy):
        # half-wave spectral flux = only energy INCREASES (attack)
        flux = np.maximum(band_energy - self._prev, 0.0)
        self._prev = band_energy

        # normalize level and flux, each per band, independently
        level_n = self.agc.update(band_energy)
        # normalize flux against the same ceiling as the level (same scale)
        flux_n = np.clip(flux / np.maximum(self.agc.peak, 1e-6), 0.0, 1.0)

        raw = (1.0 - self.flux_mix) * level_n + self.flux_mix * flux_n
        smoothed = self.env.update(raw)

        out = smoothed ** self.gamma                 # perceptual
        out[out < self.gate] = 0.0                   # noise gate
        return np.clip(out, 0.0, 1.0)

    # ---- real-time API: takes 'hop' samples, returns (N,) ----
    def push(self, samples):
        samples = np.asarray(samples, dtype=np.float32).ravel()
        # sliding window: keeps n_fft samples, advances by 'hop'
        self._buf = np.roll(self._buf, -len(samples))
        self._buf[-len(samples):] = samples
        band_energy = self._frame_to_bands(self._buf)
        return self._combine(band_energy)

    # ---- offline API: whole signal -> (times, I[T,N]) ----
    def process_offline(self, samples, sr=None, global_norm=True):
        """Processes the entire file. Since you HAVE the file (not a live
        mic), you can normalize using each band's global statistics --
        much more stable than the online AGC. global_norm=True uses each
        band's 95th percentile instead of the AGC.
        """
        samples = np.asarray(samples, dtype=np.float32).ravel()
        if sr and sr != self.sr:
            # simple (linear) resample if needed
            n_new = int(len(samples) * self.sr / sr)
            samples = np.interp(
                np.linspace(0, len(samples) - 1, n_new),
                np.arange(len(samples)), samples,
            ).astype(np.float32)

        # 1) per-band energy across every frame
        n_frames = 1 + max(0, (len(samples) - self.n_fft) // self.hop)
        E = np.zeros((n_frames, self.n_bands), dtype=np.float32)
        for k in range(n_frames):
            i = k * self.hop
            E[k] = self._frame_to_bands(samples[i:i + self.n_fft])

        # 2) per-band half-wave spectral flux (attack)
        flux = np.maximum(np.diff(E, axis=0, prepend=E[:1]), 0.0)

        # 3) per-band normalization
        if global_norm:
            ref = np.percentile(E, 95, axis=0)
            ref[ref < 1e-6] = 1e-6
            level_n = np.clip(E / ref, 0.0, 1.0)
            flux_n = np.clip(flux / ref, 0.0, 1.0)
        else:
            level_n = np.zeros_like(E)
            flux_n = np.zeros_like(E)
            agc = PerBandAGC(self.n_bands, decay=self.agc.decay)
            for k in range(n_frames):
                level_n[k] = agc.update(E[k])
                flux_n[k] = np.clip(flux[k] / np.maximum(agc.peak, 1e-6), 0, 1)

        raw = (1 - self.flux_mix) * level_n + self.flux_mix * flux_n

        # 4) attack/decay envelope + perceptual + gate
        env = ExpEnvelope(self.n_bands, self.env.alpha_attack, self.env.alpha_release)
        I = np.zeros_like(raw)
        for k in range(n_frames):
            s = env.update(raw[k])
            o = s ** self.gamma
            o[o < self.gate] = 0.0
            I[k] = np.clip(o, 0.0, 1.0)

        times = np.arange(n_frames) * self.hop / self.sr
        return times, I


# ---- reusable high-level function ------------------------------------
def compute_intensities(samples, sr, n_bands, global_norm=True, **params):
    """One-shot function: audio (mono samples, sr) -> (times, I[T, n_bands]).

    Reusable wrapper over ReactiveMapper for the offline (whole-file) case.
    Pass any ReactiveMapper parameter as a keyword: flux_mix, gamma, gate,
    attack, release, fmin, fmax, agc_decay, n_fft, hop.

        times, I = compute_intensities(y, sr, n_bands=8, flux_mix=0.6)

    I[k] is the vector of n_bands intensities (0..1) at instant times[k].
    """
    mapper = ReactiveMapper(n_bands=n_bands, sr=sr, **params)
    return mapper.process_offline(samples, sr, global_norm=global_norm)


# ---- optional mp3 loader ---------------------------------------------
def load_audio(path, sr=22050):
    """Loads an mp3/wav as mono at the desired sample rate. Requires librosa."""
    import librosa
    y, _sr = librosa.load(path, sr=sr, mono=True)
    return y.astype(np.float32), sr
