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
	Watch  []ProcessRule `yaml:"watch"`
	Ignore []string      `yaml:"ignore"`
}

type ProcessRule struct {
	Name       string `yaml:"name"`
	PathPrefix string `yaml:"path_prefix,omitempty"`
}

type Record struct {
	FollowMouse      bool   `yaml:"follow_mouse"`
	FPS              int    `yaml:"fps"`
	VideoBitrateKbps int    `yaml:"video_bitrate_kbps"`
	EncodeMaxWidth   int    `yaml:"encode_max_width"`  // 0 = 不缩小，保持采集原生分辨率
	EncodeMaxHeight  int    `yaml:"encode_max_height"` // 0 = 不缩小
	Container        string `yaml:"container"`         // wmv | mp4
}

type Audio struct {
	Enabled    bool          `yaml:"enabled"`
	System     AudioEndpoint `yaml:"system"`
	Microphone AudioEndpoint `yaml:"microphone"`
	Gain       AudioGain     `yaml:"gain"`
	ClipLimit  bool          `yaml:"clip_limit"`
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
			Ignore: []string{"SystemSettings.exe"},
		},
		Record: Record{
			FollowMouse:      true,
			FPS:              15,
			VideoBitrateKbps: 400,
			EncodeMaxWidth:   0,
			EncodeMaxHeight:  0,
			Container:        "wmv",
		},
		Audio: Audio{
			Enabled: true,
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
