"""Sensors: supply voltage, amplifier state, estimated level, stream buffer, diagnostics."""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import EntityCategory, UnitOfElectricPotential, UnitOfInformation, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import AudioBrickConfigEntry
from .coordinator import AudioBrickCoordinator
from .entity import AudioBrickEntity


@dataclass(frozen=True, kw_only=True)
class AudioBrickSensorDescription(SensorEntityDescription):
    value: Callable[[dict[str, Any], dict[str, Any]], Any]
    needs_feature: str | None = None


SENSORS: tuple[AudioBrickSensorDescription, ...] = (
    AudioBrickSensorDescription(
        key="pvdd",
        translation_key="supply_voltage",
        device_class=SensorDeviceClass.VOLTAGE,
        native_unit_of_measurement=UnitOfElectricPotential.VOLT,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        value=lambda s, b: s.get("pvdd_v") if (s.get("pvdd_v") or -1) >= 0 else None,
    ),
    AudioBrickSensorDescription(
        key="amp",
        translation_key="amp_state",
        device_class=SensorDeviceClass.ENUM,
        options=["active", "hiz", "off"],
        value=lambda s, b: s.get("amp"),
    ),
    AudioBrickSensorDescription(
        key="spl",
        translation_key="estimated_level",
        native_unit_of_measurement="dB",
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=0,
        value=lambda s, b: (s.get("est") or {}).get("spl"),
    ),
    AudioBrickSensorDescription(
        key="buffer",
        translation_key="stream_buffer",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.MILLISECONDS,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s, b: (s.get("media") or {}).get("buffer_ms"),
    ),
    AudioBrickSensorDescription(
        key="dropouts",
        translation_key="dropouts",
        state_class=SensorStateClass.TOTAL_INCREASING,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s, b: (s.get("media") or {}).get("underruns"),
    ),
    AudioBrickSensorDescription(
        key="uptime",
        translation_key="uptime",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        state_class=SensorStateClass.TOTAL_INCREASING,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda s, b: s.get("uptime_s"),
    ),
    AudioBrickSensorDescription(
        key="free_heap",
        translation_key="free_heap",
        device_class=SensorDeviceClass.DATA_SIZE,
        native_unit_of_measurement=UnitOfInformation.BYTES,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda s, b: (s.get("heap") or {}).get("free"),
    ),
    AudioBrickSensorDescription(
        key="restart_reason",
        translation_key="restart_reason",
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda s, b: (s.get("boot") or {}).get("reason"),
    ),
    AudioBrickSensorDescription(
        key="bluetooth_device",
        translation_key="bluetooth_device",
        needs_feature="bluetooth",
        value=lambda s, b: (b.get("peer_name") or None) if b.get("connected") else None,
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback) -> None:
    coordinator = entry.runtime_data.coordinator
    features = coordinator.data["status"].get("features", {})
    async_add_entities(
        AudioBrickSensor(coordinator, d) for d in SENSORS if d.needs_feature is None or features.get(d.needs_feature)
    )


class AudioBrickSensor(AudioBrickEntity, SensorEntity):
    entity_description: AudioBrickSensorDescription

    def __init__(self, coordinator: AudioBrickCoordinator, description: AudioBrickSensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value(self.status, self.bluetooth)
