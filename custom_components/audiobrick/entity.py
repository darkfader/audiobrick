"""Base class: one device per Brick."""
from __future__ import annotations

from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import AudioBrickCoordinator


class AudioBrickEntity(CoordinatorEntity[AudioBrickCoordinator]):
    """Entity of the Brick; shares the device and the polled data."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: AudioBrickCoordinator, key: str) -> None:
        super().__init__(coordinator)
        status = coordinator.data["status"]
        self._ident = status.get("mac") or coordinator.client.host
        self._attr_unique_id = f"{self._ident}_{key}"
        connections = {(CONNECTION_NETWORK_MAC, status["mac"])} if status.get("mac") else set()
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, self._ident)},
            connections=connections,
            manufacturer="Sonocotta (community firmware)",
            model="Esparagus Audio Brick (ESP32)",
            name="Audio Brick",
            sw_version=status.get("version"),
            configuration_url=coordinator.client.base_url,
        )

    @property
    def status(self) -> dict:
        return self.coordinator.data["status"]

    @property
    def bluetooth(self) -> dict:
        return self.coordinator.data["bluetooth"]
