"""The Esparagus Audio Brick integration (a networked speaker with a TAS5825M amplifier)."""
from __future__ import annotations

from dataclasses import dataclass

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import ConfigEntryNotReady, HomeAssistantError
from homeassistant.helpers import config_validation as cv, device_registry as dr
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import AudioBrickClient, AudioBrickError
from .const import CONF_HOST, CONF_PASSWORD, DOMAIN
from .coordinator import AudioBrickCoordinator

PLATFORMS = [Platform.MEDIA_PLAYER, Platform.SENSOR, Platform.BINARY_SENSOR, Platform.SWITCH, Platform.BUTTON]

SERVICE_SLEEP_TIMER = "sleep_timer"
ATTR_MINUTES = "minutes"


@dataclass
class AudioBrickData:
    client: AudioBrickClient
    coordinator: AudioBrickCoordinator


type AudioBrickConfigEntry = ConfigEntry[AudioBrickData]


async def async_setup_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry) -> bool:
    client = AudioBrickClient(async_get_clientsession(hass), entry.data[CONF_HOST], entry.data[CONF_PASSWORD])
    coordinator = AudioBrickCoordinator(hass, entry, client)
    await coordinator.async_config_entry_first_refresh()
    if not coordinator.data:
        raise ConfigEntryNotReady
    entry.runtime_data = AudioBrickData(client=client, coordinator=coordinator)

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    if not hass.services.has_service(DOMAIN, SERVICE_SLEEP_TIMER):
        async def _sleep_timer(call: ServiceCall) -> None:
            minutes = call.data[ATTR_MINUTES]
            targets = _targets(hass, call)
            for target in targets:
                try:
                    await target.client.sleep_timer(minutes)
                except AudioBrickError as err:
                    raise HomeAssistantError(str(err)) from err
                await target.coordinator.async_request_refresh()

        hass.services.async_register(
            DOMAIN,
            SERVICE_SLEEP_TIMER,
            _sleep_timer,
            schema=vol.Schema(
                {
                    vol.Required(ATTR_MINUTES): vol.All(vol.Coerce(int), vol.Range(min=0, max=720)),
                    vol.Optional("device_id"): vol.All(cv.ensure_list, [cv.string]),
                }
            ),
        )
    return True


def _targets(hass: HomeAssistant, call: ServiceCall) -> list[AudioBrickData]:
    """The Bricks a service call is aimed at (the devices given, or all of them)."""
    entries = hass.config_entries.async_entries(DOMAIN)
    wanted = call.data.get("device_id")
    if wanted:
        registry = dr.async_get(hass)
        ids = set()
        for device_id in wanted:
            device = registry.async_get(device_id)
            if device:
                ids.update(device.config_entries)
        entries = [e for e in entries if e.entry_id in ids]
    return [e.runtime_data for e in entries if getattr(e, "runtime_data", None)]


async def async_unload_entry(hass: HomeAssistant, entry: AudioBrickConfigEntry) -> bool:
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded and not hass.config_entries.async_loaded_entries(DOMAIN):
        hass.services.async_remove(DOMAIN, SERVICE_SLEEP_TIMER)
    return unloaded
