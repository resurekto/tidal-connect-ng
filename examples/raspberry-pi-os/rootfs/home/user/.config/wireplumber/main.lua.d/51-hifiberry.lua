-- HiFiBerry DAC+: use alsa-card-profile/mixer/profile-sets/hifiberry.conf (WirePlumber 0.4).
table.insert(alsa_monitor.rules, {
  matches = { { { "api.alsa.card.name", "equals", "snd_rpi_hifiberry_dacplus" } } },
  apply_properties = { ["device.profile-set"] = "hifiberry.conf" },
})
