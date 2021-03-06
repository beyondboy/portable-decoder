// decoder/signal.h
// wujian@2018.8

// Compute spectrogram/fbank/mfcc acoustic features according to Kaldi's logic
// with less dependency
//

#ifndef SIGNAL_H
#define SIGNAL_H

#include "decoder/common.h"
#include "decoder/config.h"
#include "decoder/fft-computer.h"

// Preemphasize function
void Preemphasize(Float32 *frame, Int32 frame_length, Float32 preemph_coeff);

// Map WindowType to string
std::string WindowToString(WindowType window);

// Map string to WindowType
WindowType StringToWindow(const std::string &window);

// Compute window values
void ComputeWindow(Int32 window_size, Float32 *window, WindowType window_type);

// Compute power/magnitude spectrum from Realfft results
void ComputeSpectrum(Float32 *realfft, Int32 dim, Float32 *spectrum,
                     Bool apply_pow, Bool apply_log);

// Compute mel-filter coefficients
void ComputeMelFilters(Int32 num_fft_bins, Int32 num_mel_bins,
                       Int32 sample_rate, Int32 lower_bound, Int32 upper_bound,
                       std::vector<std::vector<Float32> > *weights);

// Compute DCT transform matrix
void ComputeDctMatrix(Float32 *dct_matrix_, Int32 num_rows, Int32 num_cols);

class Options {
 public:
  virtual void Check() const = 0;
  virtual void ParseConfigure(ConfigureParser *parser) = 0;
  virtual std::string Configure() = 0;
};

// Each feature computer implement following functions:
// FeatureDim(): which gives dimention of features
// NumFrames(): which work out number of frames
// ComputeFrame(): which compute feature for a single frame
class Computer {
 public:
  virtual Float32 ComputeFrame(Float32 *signal, Int32 num_samps, Int32 t,
                               Float32 *spectrum_addr) = 0;
  virtual Int32 FeatureDim() = 0;
  virtual Int32 NumFrames(Int32 num_samps) = 0;
  virtual void Reset() = 0;
  virtual ~Computer(){};
};

// template<class Computer>
Int32 ComputeFeature(Computer *computer, Float32 *signal, Int32 num_samps,
                     Float32 *addr, Int32 stride);

// In order to use in Cython, I make it class instead of struct
class FrameOpts : public Options {
 public:
  Int32 frame_length, frame_shift, sample_rate;
  WindowType window_type;
  Float32 preemph_coeff;
  Bool remove_dc;

  FrameOpts(Int32 length = 400, Int32 shift = 160, Int32 frequency = 16000,
            Float32 coeff = 0.97, WindowType window = kHamm,
            Bool remove_dc = true)
      : frame_length(length),
        frame_shift(shift),
        sample_rate(frequency),
        window_type(window),
        preemph_coeff(coeff),
        remove_dc(remove_dc) {}

  FrameOpts(const FrameOpts &opts)
      : frame_length(opts.frame_length),
        frame_shift(opts.frame_shift),
        sample_rate(opts.sample_rate),
        window_type(opts.window_type),
        preemph_coeff(opts.preemph_coeff),
        remove_dc(opts.remove_dc) {}

  void Check() const {
    ASSERT(sample_rate && frame_length >= frame_shift);
    ASSERT(preemph_coeff >= 0 && preemph_coeff < 1.0);
  }

  void ParseConfigure(ConfigureParser *parser) {
    parser->AddOptions("FrameOpts", "frame_length", &frame_length);
    parser->AddOptions("FrameOpts", "frame_shift", &frame_shift);
    parser->AddOptions("FrameOpts", "preemph_coeff", &preemph_coeff);
    parser->AddOptions("FrameOpts", "sample_rate", &sample_rate);
    parser->AddOptions("FrameOpts", "remove_dc", &remove_dc);
    std::string window;
    parser->AddOptions("FrameOpts", "window", &window);
    window_type = StringToWindow(window);
  }

  std::string Configure() {
    std::ostringstream oss;
    oss << "--FrameOpts.frame_length=" << frame_length << std::endl;
    oss << "--FrameOpts.frame_shift=" << frame_shift << std::endl;
    oss << "--FrameOpts.preemph_coeff=" << preemph_coeff << std::endl;
    oss << "--FrameOpts.sample_rate=" << sample_rate << std::endl;
    oss << "--FrameOpts.remove_dc=" << (remove_dc ? "true" : "false")
        << std::endl;
    oss << "--FrameOpts.window=" << WindowToString(window_type) << std::endl;
    return oss.str();
  }
};

// Using for feature extraction
// I make it support for online scenario
class FrameSplitter {
 public:
  FrameSplitter(const FrameOpts &opts) : frame_opts_(opts), window_(NULL) {
    Reset();
    frame_opts_.Check();
    online_use_ = new Float32[frame_opts_.frame_length * 2];
    if (frame_opts_.window_type != kNone) {
      window_ = new Float32[frame_opts_.frame_length];
      ComputeWindow(frame_opts_.frame_length, window_, frame_opts_.window_type);
    }
  }

  // Framing whole signal at a time into assigned memory address
  Int32 Frame(Float32 *signal, Int32 num_samps, Float32 *frames, Int32 stride) {
    ASSERT(frame_opts_.frame_length <= stride);
    Int32 num_frames = NumFrames(num_samps);
    for (Int32 t = 0; t < num_frames; t++)
      FrameForIndex(signal, num_samps, t, frames + stride * t, NULL);
    return num_frames;
  }

  void Reset() { prev_discard_size_ = 0; }

  // Copy Frame at time 'index' into assigned memory address
  void FrameForIndex(Float32 *signal, Int32 num_samps, Int32 index,
                     Float32 *frame, Float32 *raw_energy);

  // Compute number of frames given number of samples
  // Consider online scenario here
  Int32 NumFrames(Int32 num_samps) {
    Int32 num_frames = static_cast<Int32>((num_samps + prev_discard_size_ -
                                           frame_opts_.frame_length) /
                                          frame_opts_.frame_shift) +
                       1;
    if (num_frames == 0)
      LOG_WARN << "Number of samples is less than frame length, "
               << num_samps + prev_discard_size_ << " vs "
               << frame_opts_.frame_length;
    return num_frames;
  }

  Int32 FrameLength() { return frame_opts_.frame_length; }

  Int32 FrameShift() { return frame_opts_.frame_shift; }

  Int32 SampleRate() { return frame_opts_.sample_rate; }

  Int32 PaddingLength() {
    return RoundUpToNearestPowerOfTwo(frame_opts_.frame_length);
  }

  ~FrameSplitter() {
    if (window_) delete[] window_;
    if (online_use_) delete[] online_use_;
  }

 private:
  void FixFrame(Float32 *signal, Int32 t, Float32 *frame_addr);

  FrameOpts frame_opts_;
  Float32 *window_;
  // online_use_[0: prev_discard_size_]: cache previous discard samples
  // online_use_[prev_discard_size_: ]:  fix first frames
  Float32 *online_use_;  // for online use
  Int32 prev_discard_size_;
};

class SpectrogramOpts : public Options {
 public:
  Bool apply_log, apply_pow, use_log_raw_energy;
  // Log-spectrogram or Linear-spectrogram
  // Power-spectrum or Magnitude spectrum
  // Replace S[0] using log-energy or not
  FrameOpts frame_opts;

  SpectrogramOpts(const FrameOpts &opts)
      : apply_log(true),
        apply_pow(true),
        use_log_raw_energy(true),
        frame_opts(opts) {}

  SpectrogramOpts(Bool power = true, Bool log = true, Bool use_energy = true)
      : apply_log(log), apply_pow(power), use_log_raw_energy(use_energy) {}

  SpectrogramOpts(const SpectrogramOpts &opts)
      : apply_log(opts.apply_log),
        apply_pow(opts.apply_pow),
        use_log_raw_energy(opts.use_log_raw_energy),
        frame_opts(opts.frame_opts) {}

  void Check() const { frame_opts.Check(); }

  void ParseConfigure(ConfigureParser *parser) {
    frame_opts.ParseConfigure(parser);
    parser->AddOptions("SpectrogramOpts", "apply_log", &apply_log);
    parser->AddOptions("SpectrogramOpts", "apply_pow", &apply_pow);
    parser->AddOptions("SpectrogramOpts", "use_log_raw_energy",
                       &use_log_raw_energy);
  }

  std::string Configure() {
    std::ostringstream oss;
    oss << frame_opts.Configure();
    oss << "--SpectrogramOpts.apply_log=" << (apply_log ? "true" : "false")
        << std::endl;
    oss << "--SpectrogramOpts.apply_pow=" << (apply_pow ? "true" : "false")
        << std::endl;
    oss << "--SpectrogramOpts.use_log_raw_energy="
        << (use_log_raw_energy ? "true" : "false") << std::endl;
    return oss.str();
  }
};

// SpectrogramComputer for ASR
class SpectrogramComputer : public Computer {
 public:
  SpectrogramComputer(const SpectrogramOpts &spectrogram_opts)
      : apply_pow_(spectrogram_opts.apply_pow),
        apply_log_(spectrogram_opts.apply_log),
        use_log_raw_energy_(spectrogram_opts.use_log_raw_energy),
        splitter(spectrogram_opts.frame_opts) {
    padding_length_ = splitter.PaddingLength();
    fft_computer = new FFTComputer(padding_length_);
    realfft_cache_ = new Float32[padding_length_];
  }

  // Compute spectrum for frame t
  Float32 ComputeFrame(Float32 *signal, Int32 num_samps, Int32 t,
                       Float32 *spectrum_addr);

  ~SpectrogramComputer() {
    if (fft_computer) delete fft_computer;
    if (realfft_cache_) delete[] realfft_cache_;
  }

  Int32 PaddingLength() { return padding_length_; }

  Int32 FeatureDim() { return padding_length_ / 2 + 1; }

  Int32 NumFrames(Int32 num_samps) { return splitter.NumFrames(num_samps); }

  void Reset() { splitter.Reset(); }

 protected:
  Bool apply_pow_, apply_log_, use_log_raw_energy_;
  Int32 padding_length_;
  FrameSplitter splitter;
  FFTComputer *fft_computer;
  Float32 *realfft_cache_;
};

class FbankOpts : public Options {
 public:
  Int32 num_mel_bins;              // Number of mel bins/Feature dim
  Int32 lower_bound, upper_bound;  // lower/upper frequency bound
  Bool apply_log;                  // Apply log on mel-energy
  SpectrogramOpts spectrogram_opts;

  FbankOpts(const SpectrogramOpts &opts)
      : num_mel_bins(23),
        lower_bound(20),
        upper_bound(0),
        apply_log(true),
        spectrogram_opts(opts) {}

  FbankOpts(Int32 num_bins = 23, Int32 low = 20, Int32 high = 0,
            Bool power = true, Bool log = true)
      : num_mel_bins(num_bins),
        lower_bound(low),
        upper_bound(high),
        apply_log(log) {
    spectrogram_opts.apply_log = spectrogram_opts.use_log_raw_energy = false;
  }

  void Check() const {
    spectrogram_opts.Check();
    ASSERT(num_mel_bins >= 3);
    ASSERT(lower_bound >= 0);
  }

  void ParseConfigure(ConfigureParser *parser) {
    spectrogram_opts.ParseConfigure(parser);
    spectrogram_opts.apply_log = spectrogram_opts.use_log_raw_energy = false;
    parser->AddOptions("FbankOpts", "num_mel_bins", &num_mel_bins);
    parser->AddOptions("FbankOpts", "lower_bound", &lower_bound);
    parser->AddOptions("FbankOpts", "upper_bound", &upper_bound);
    parser->AddOptions("FbankOpts", "apply_log", &apply_log);
  }

  std::string Configure() {
    std::ostringstream oss;
    oss << spectrogram_opts.Configure();
    oss << "--FbankOpts.apply_log=" << (apply_log ? "true" : "false")
        << std::endl;
    oss << "--FbankOpts.lower_bound=" << lower_bound << std::endl;
    oss << "--FbankOpts.upper_bound=" << upper_bound << std::endl;
    oss << "--FbankOpts.num_mel_bins=" << num_mel_bins << std::endl;
    return oss.str();
  }
};

// FbankComputer use SpectrogramComputer to extract linear-spectrogram
class FbankComputer : public Computer {
 public:
  FbankComputer(const FbankOpts &fbank_opts)
      : num_bins_(fbank_opts.num_mel_bins),
        lower_bound_(fbank_opts.lower_bound),
        apply_log_(fbank_opts.apply_log),
        spectrogram_computer_(fbank_opts.spectrogram_opts) {
    // Check configures
    fbank_opts.Check();
    SpectrogramOpts spectrogram_opts(fbank_opts.spectrogram_opts);
    ASSERT(!spectrogram_opts.apply_log && !spectrogram_opts.use_log_raw_energy);
    Int32 center_freq = spectrogram_opts.frame_opts.sample_rate >> 1;
    // spectrogram_computer_ = new SpectrogramComputer(spectrogram_opts);
    spectrum_cache_ = new Float32[spectrogram_computer_.FeatureDim()];
    upper_bound_ = fbank_opts.upper_bound > 0.0
                       ? fbank_opts.upper_bound
                       : center_freq + fbank_opts.upper_bound;
    ComputeMelFilters(spectrogram_computer_.FeatureDim(), num_bins_,
                      center_freq, lower_bound_, upper_bound_, &mel_coeff_);
  }

  ~FbankComputer() {
    if (spectrum_cache_) delete[] spectrum_cache_;
  }

  void Reset() { spectrogram_computer_.Reset(); }

  Float32 ComputeFrame(Float32 *signal, Int32 num_samps, Int32 t,
                       Float32 *fbank_addr);

  Int32 FeatureDim() { return num_bins_; }

  Int32 NumFrames(Int32 num_samps) {
    return spectrogram_computer_.NumFrames(num_samps);
  }

 protected:
  Float32 *spectrum_cache_;
  Int32 num_bins_, lower_bound_, upper_bound_;
  Bool apply_log_;
  std::vector<std::vector<Float32> > mel_coeff_;
  SpectrogramComputer spectrogram_computer_;
};

class MfccOpts : public Options {
 public:
  FbankOpts fbank_opts;

  Int32 num_ceps;   // Feature dim
  Bool use_energy;  // Replace C0 using energy
  Float32 cepstral_lifter;

  MfccOpts(Int32 num_ceps = 13, Bool energy = true, Float32 cepstral = 22.0)
      : num_ceps(num_ceps), use_energy(energy), cepstral_lifter(cepstral) {
    fbank_opts.spectrogram_opts.apply_pow = fbank_opts.apply_log = true;
  }

  MfccOpts(const FbankOpts opts)
      : fbank_opts(opts),
        num_ceps(13),
        use_energy(true),
        cepstral_lifter(23.0) {}

  void Check() const {
    fbank_opts.Check();
    ASSERT(num_ceps >= 1);
  }

  void ParseConfigure(ConfigureParser *parser) {
    fbank_opts.ParseConfigure(parser);
    fbank_opts.spectrogram_opts.apply_pow = fbank_opts.apply_log = true;
    parser->AddOptions("MfccOpts", "num_ceps", &num_ceps);
    parser->AddOptions("MfccOpts", "use_energy", &use_energy);
    parser->AddOptions("MfccOpts", "cepstral_lifter", &cepstral_lifter);
  }

  std::string Configure() {
    std::ostringstream oss;
    oss << fbank_opts.Configure();
    oss << "--MfccOpts.num_ceps=" << num_ceps << std::endl;
    oss << "--MfccOpts.use_energy=" << (use_energy ? "true" : "false")
        << std::endl;
    oss << "--MfccOpts.cepstral_lifter=" << cepstral_lifter << std::endl;
    return oss.str();
  }
};

class MfccComputer : public Computer {
 public:
  MfccComputer(const MfccOpts &mfcc_opts)
      : num_ceps_(mfcc_opts.num_ceps),
        use_energy_(mfcc_opts.use_energy),
        cepstral_lifter_(mfcc_opts.cepstral_lifter),
        fbank_computer(mfcc_opts.fbank_opts) {
    mfcc_opts.Check();
    lifter_coeffs_ = new Float32[num_ceps_];
    // Compute liftering coefficients
    for (Int32 i = 0; i < num_ceps_; i++)
      lifter_coeffs_[i] =
          0.5 * cepstral_lifter_ * sin(PI * i / cepstral_lifter_) + 1.0;
    ASSERT(mfcc_opts.fbank_opts.spectrogram_opts.apply_pow &&
           mfcc_opts.fbank_opts.apply_log);
    // Use FbankComputer to compute mel-energy
    // fbank_computer = new FbankComputer(mfcc_opts.fbank_opts);
    Int32 num_mel_bins = fbank_computer.FeatureDim();
    // Allocate memory for DCT matrix, only use first num_ceps rows
    dct_matrix_ = new Float32[num_ceps_ * num_mel_bins];
    ComputeDctMatrix(dct_matrix_, num_ceps_, num_mel_bins);
    mel_energy_cache_ = new Float32[num_mel_bins];
  }

  ~MfccComputer() {
    if (lifter_coeffs_) delete[] lifter_coeffs_;
    if (dct_matrix_) delete[] dct_matrix_;
    if (mel_energy_cache_) delete[] mel_energy_cache_;
  }

  void Reset() { fbank_computer.Reset(); }

  Float32 ComputeFrame(Float32 *signal, Int32 num_samps, Int32 t,
                       Float32 *mfcc_addr);

  Int32 FeatureDim() { return num_ceps_; }

  Int32 NumFrames(Int32 num_samps) {
    return fbank_computer.NumFrames(num_samps);
  }

 private:
  Int32 num_ceps_;
  Bool use_energy_;
  Float32 cepstral_lifter_, *lifter_coeffs_, *dct_matrix_, *mel_energy_cache_;
  FbankComputer fbank_computer;
};

#endif