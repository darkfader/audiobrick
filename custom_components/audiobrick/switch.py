"""Switches: the ambient sound scene and Bluetooth."""
from __future__ import annotations

from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.switch import SwitchEntity, SwitchEntityDescription
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import AudioBrickConfigEntry
from .api import AudioBrickClient, AudioBrickError
from .coordinator import AudioBrickCoordinator
from .entity import AudioBrickEntity


@dataclass(frozen=True, kw_only=True)
class AudioBrickSwitchDescription(SwitchEntityDescription):
    is_on: Callable[[dict[str, Any], dict[str, Any]], bool]
    set_on: Callable[[AudioBrickClient, bool], Awaitable[None]]
    needs_feature: str


SWITCHES: tuple[AudioBrickSwitchDescription, ...] = (
    AudioBrickSwitchDescription(
        key="ambient",
        translation_key="ambient_scene",
        needs_feature="ambient",
        is_on=lambda s, b: bool((s.get("ambient") or {}).get("on")),
        set_on=lambda c, on: c.set_ambient(on),
    ),
    AudioBrickSwitchDescription(
        key="bluetooth",
        translation_key="bluetooth",
        needs_feature="bluetooth",
        is_on=lambda s, b: bool(b.get("enabled")),
        set_on=lambda c, on: c.set_bluetooth(on),
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback) -> None:
    coordinator = entry.runtime_data.coordinator
    features = coordinator.data["status"].get("features", {})
    async_add_entities(AudioBrickSwitch(coordinator, d) for d in SWITCHES if features.get(d.needs_feature))


class AudioBrickSwitch(AudioBrickEntity, SwitchEntity):
    entity_description: AudioBrickSwitchDescription

    def __init__(self, coordinator: AudioBrickCoordinator, description: AudioBrickSwitchDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool:
        return self.entity_description.is_on(self.status, self.bluetooth)

    async def _set(self, on: bool) -> None:
        try:
            await self.entity_description.set_on(self.coordinator.client, on)
        except AudioBrickError as err:
            raise HomeAssistantError(str(err)) from err
        await self.coordinator.async_request_refresh()

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._set(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._set(False)
