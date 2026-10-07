"""Buttons: stop everything, Bluetooth pairing window and disconnect."""
from __future__ import annotations

from collections.abc import Awaitable, Callable
from dataclasses import dataclass

from homeassistant.components.button import ButtonEntity, ButtonEntityDescription
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import AudioBrickConfigEntry
from .api import AudioBrickClient, AudioBrickError
from .coordinator import AudioBrickCoordinator
from .entity import AudioBrickEntity


@dataclass(frozen=True, kw_only=True)
class AudioBrickButtonDescription(ButtonEntityDescription):
    press: Callable[[AudioBrickClient], Awaitable[None]]
    needs_feature: str | None = None


BUTTONS: tuple[AudioBrickButtonDescription, ...] = (
    AudioBrickButtonDescription(
        key="stop",
        translation_key="stop",
        press=lambda c: c.stop(),
    ),
    AudioBrickButtonDescription(
        key="bluetooth_pair",
        translation_key="bluetooth_pair",
        needs_feature="bluetooth",
        press=lambda c: c.bluetooth_pairing(True),
    ),
    AudioBrickButtonDescription(
        key="bluetooth_disconnect",
        translation_key="bluetooth_disconnect",
        needs_feature="bluetooth",
        press=lambda c: c.bluetooth_disconnect(),
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback) -> None:
    coordinator = entry.runtime_data.coordinator
    features = coordinator.data["status"].get("features", {})
    async_add_entities(
        AudioBrickButton(coordinator, d) for d in BUTTONS if d.needs_feature is None or features.get(d.needs_feature)
    )


class AudioBrickButton(AudioBrickEntity, ButtonEntity):
    entity_description: AudioBrickButtonDescription

    def __init__(self, coordinator: AudioBrickCoordinator, description: AudioBrickButtonDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    async def async_press(self) -> None:
        try:
            await self.entity_description.press(self.coordinator.client)
        except AudioBrickError as err:
            raise HomeAssistantError(str(err)) from err
        await self.coordinator.async_request_refresh()
