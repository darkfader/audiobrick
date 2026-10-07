"""Polls the Audio Brick."""
from __future__ import annotations

from datetime import timedelta
import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import AudioBrickAuthError, AudioBrickClient, AudioBrickError
from .const import DOMAIN, SCAN_INTERVAL_SECONDS

_LOGGER = logging.getLogger(__name__)

CLIPS_EVERY = 12  # refresh the clip list every 12th poll (a minute)


class AudioBrickCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Fetches /status every few seconds, plus Bluetooth state and the clip list when the firmware has them."""

    config_entry: ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, client: AudioBrickClient) -> None:
        super().__init__(
            hass,
            _LOGGER,
            config_entry=entry,
            name=DOMAIN,
            update_interval=timedelta(seconds=SCAN_INTERVAL_SECONDS),
        )
        self.client = client
        self._polls = 0
        self._clips: list[str] = []

    async def _async_update_data(self) -> dict[str, Any]:
        try:
            status = await self.client.status()
            features = status.get("features", {})
            bluetooth: dict[str, Any] = {}
            if features.get("bluetooth"):
                bluetooth = await self.client.bluetooth()
            if features.get("clips") and (self._polls % CLIPS_EVERY == 0 or not self._clips):
                self._clips = await self.client.clips()
            self._polls += 1
        except AudioBrickAuthError as err:
            raise ConfigEntryAuthFailed(str(err)) from err
        except AudioBrickError as err:
            raise UpdateFailed(str(err)) from err
        return {"status": status, "bluetooth": bluetooth, "clips": list(self._clips)}
