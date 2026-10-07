"""Binary sensors: amplifier fault and warning, safe mode, Bluetooth connection."""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import AudioBrickConfigEntry
from .coordinator import AudioBrickCoordinator
from .entity import AudioBrickEntity


@dataclass(frozen=True, kw_only=True)
class AudioBrickBinarySensorDescription(BinarySensorEntityDescription):
    value: Callable[[dict[str, Any], dict[str, Any]], bool]
    needs_feature: str | None = None


BINARY_SENSORS: tuple[AudioBrickBinarySensorDescription, ...] = (
    AudioBrickBinarySensorDescription(
        key="fault",
        translation_key="amp_fault",
        device_class=BinarySensorDeviceClass.PROBLEM,
        value=lambda s, b: bool(s.get("fault")),
    ),
    AudioBrickBinarySensorDescription(
        key="warning",
        translation_key="amp_warning",
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s, b: bool(s.get("warning")),
    ),
    AudioBrickBinarySensorDescription(
        key="safe_mode",
        translation_key="safe_mode",
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s, b: bool(s.get("safe_mode")),
    ),
    AudioBrickBinarySensorDescription(
        key="bluetooth_connected",
        translation_key="bluetooth_connected",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        needs_feature="bluetooth",
        value=lambda s, b: bool(b.get("connected")),
    ),
    AudioBrickBinarySensorDescription(
        key="bluetooth_pairing",
        translation_key="bluetooth_pairing",
        needs_feature="bluetooth",
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda s, b: bool(b.get("pairing")),
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback) -> None:
    coordinator = entry.runtime_data.coordinator
    features = coordinator.data["status"].get("features", {})
    async_add_entities(
        AudioBrickBinarySensor(coordinator, d) for d in BINARY_SENSORS if d.needs_feature is None or features.get(d.needs_feature)
    )


class AudioBrickBinarySensor(AudioBrickEntity, BinarySensorEntity):
    entity_description: AudioBrickBinarySensorDescription

    def __init__(self, coordinator: AudioBrickCoordinator, description: AudioBrickBinarySensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool:
        return self.entity_description.value(self.status, self.bluetooth)
