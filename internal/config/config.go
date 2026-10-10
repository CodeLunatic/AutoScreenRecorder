package config

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

type Config struct {
	MicTrigger MicTrigger `yaml:"mic_trigger"`
	Record     Record     `yaml:"record"`
	Audio      Audio      `yaml:"audio"`
	Output     Output     `yaml:"output"`
	Service    Service    `yaml:"service"`
}

type MicTrigger struct {
	Watch []ProcessRule `yaml:"watch"`
}

type ProcessRule struct {
	Name       string `yaml:"name"`
	PathPrefix string `yaml:"path_prefix,omitempty"`
}

type Record struct {
	FollowMouse          bool   `yaml:"follow_mouse"`
	FPS                  int    `yaml:"fps"`
	VideoBitrateKbps     int    `yaml:"video_bitrate_kbps"`
	VideoPeakBitrateKbps int    `yaml:"video_peak_bitrate_kbps"` // 0 = 平均码率的两倍
	BitrateMode          string `yaml:"bitrate_mode"`            // vbr | cbr
	Codec                string `yaml:"codec"`                   // hevc | h264；容器为 wmv 时忽略
	KeyframeSec          int    `yaml:"keyframe_sec"`            // 0 = 5 秒
	EncodeMaxWidth       int    `yaml:"encode_max_width"`        // 0 = 不缩小，保持采集原生分辨率
	EncodeMaxHeight      int    `yaml:"encode_max_height"`       // 0 = 不缩小
	Container            string `yaml:"container"`               // wmv | mp4
}

type Audio struct {
	Enabled     bool          `yaml:"enabled"`
	BitrateKbps int           `yaml:"bitrate_kbps"` // 0 = 320
	SampleRate  int           `yaml:"sample_rate"`  // 0 = 48000；44100 或 48000
	Channels    int           `yaml:"channels"`     // 0 = 2；1 或 2
	BitrateMode string        `yaml:"bitrate_mode"` // vbr | cbr。WMV 仍按固定码率
	System      AudioEndpoint `yaml:"system"`
	Microphone  AudioEndpoint `yaml:"microphone"`
	Gain        AudioGain     `yaml:"gain"`
	ClipLimit   bool          `yaml:"clip_limit"`
}

type AudioEndpoint struct {
	Enabled bool `yaml:"enabled"`
}

type AudioGain struct {
	SystemDB     float64 `yaml:"system_db"`
	MicrophoneDB float64 `yaml:"microphone_db"`
}

type Output struct {
	Dir      string `yaml:"dir"`
	Template string `yaml:"template"`
}

type Service struct {
	StopDelaySec  int `yaml:"stop_delay_sec"`
	StartDelaySec int `yaml:"start_delay_sec"`
}

func Default() Config {
	return Config{
		MicTrigger: MicTrigger{
			Watch: []ProcessRule{
				{Name: "Lark.exe"},
				{Name: "Feishu.exe"},
			},
		},
		Record: Record{
			FollowMouse:          true,
			FPS:                  15,
			VideoBitrateKbps:     400,
			VideoPeakBitrateKbps: 0,
			BitrateMode:          "vbr",
			Codec:                "hevc",
			KeyframeSec:          5,
			EncodeMaxWidth:       0,
			EncodeMaxHeight:      0,
			Container:            "wmv",
		},
		Audio: Audio{
			Enabled:     true,
			BitrateKbps: 320,
			SampleRate:  48000,
			Channels:    2,
			BitrateMode: "vbr",
			System: AudioEndpoint{
				Enabled: true,
			},
			Microphone: AudioEndpoint{
				Enabled: true,
			},
			Gain: AudioGain{
				SystemDB:     0,
				MicrophoneDB: -3,
			},
			ClipLimit: true,
		},
		Output: Output{
			Dir:      filepath.Join(os.Getenv("USERPROFILE"), "Videos", "AutoScreenRecorder"),
			Template: "{datetime}_{process}.wmv",
		},
		Service: Service{
			StopDelaySec:  5,
			StartDelaySec: 1,
		},
	}
}

// ErrFileMissing means path does not exist and Default was returned.
var ErrFileMissing = errors.New("config file missing, using defaults")

func Load(path string) (Config, error) {
	cfg := Default()
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return cfg, ErrFileMissing
		}
		return cfg, err
	}
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return cfg, err
	}
	return cfg, nil
}

func (c *Config) Validate() error {
	if c.Record.FPS <= 0 || c.Record.FPS > 60 {
		return fmt.Errorf("record.fps must be 1..60")
	}
	if c.Record.VideoBitrateKbps < 0 {
		return fmt.Errorf("record.video_bitrate_kbps must be >= 0")
	}
	mean := c.Record.VideoBitrateKbps
	if mean <= 0 {
		mean = 400
	}
	if c.Record.VideoPeakBitrateKbps < 0 {
		return fmt.Errorf("record.video_peak_bitrate_kbps must be >= 0")
	}
	if c.Record.VideoPeakBitrateKbps > 0 && c.Record.VideoPeakBitrateKbps < mean {
		return fmt.Errorf("record.video_peak_bitrate_kbps must be >= video_bitrate_kbps")
	}
	switch strings.ToLower(strings.TrimSpace(c.Record.BitrateMode)) {
	case "", "vbr", "cbr":
	default:
		return fmt.Errorf("record.bitrate_mode must be vbr or cbr")
	}
	switch strings.ToLower(strings.TrimSpace(c.Record.Codec)) {
	case "", "hevc", "h265", "h264", "avc":
	default:
		return fmt.Errorf("record.codec must be hevc or h264")
	}
	if c.Record.KeyframeSec < 0 || c.Record.KeyframeSec > 30 {
		return fmt.Errorf("record.keyframe_sec must be 0..30")
	}
	if c.Audio.BitrateKbps < 0 || c.Audio.BitrateKbps > 512 {
		return fmt.Errorf("audio.bitrate_kbps must be 0..512")
	}
	switch c.Audio.SampleRate {
	case 0, 44100, 48000:
	default:
		return fmt.Errorf("audio.sample_rate must be 0, 44100, or 48000")
	}
	switch c.Audio.Channels {
	case 0, 1, 2:
	default:
		return fmt.Errorf("audio.channels must be 0, 1, or 2")
	}
	switch strings.ToLower(strings.TrimSpace(c.Audio.BitrateMode)) {
	case "", "vbr", "cbr":
	default:
		return fmt.Errorf("audio.bitrate_mode must be vbr or cbr")
	}
	if c.Service.StopDelaySec < 0 {
		return fmt.Errorf("service.stop_delay_sec must be >= 0")
	}
	if c.Service.StartDelaySec < 0 {
		return fmt.Errorf("service.start_delay_sec must be >= 0")
	}
	return nil
}

func (c *Config) ExpandOutputDir() string {
	dir := c.Output.Dir
	dir = strings.ReplaceAll(dir, "%USERPROFILE%", os.Getenv("USERPROFILE"))
	return dir
}
