"""Config flow: add a Brick by name or address (or accept it when it is discovered)."""
from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import AudioBrickAuthError, AudioBrickClient, AudioBrickError
from .const import CONF_HOST, CONF_PASSWORD, DEFAULT_HOST, DOMAIN


class AudioBrickConfigFlow(ConfigFlow, domain=DOMAIN):
    """Ask for the address and the web password, check both, and use the Brick's MAC address as its identity."""

    VERSION = 1

    def __init__(self) -> None:
        self._host: str = DEFAULT_HOST

    async def _check(self, host: str, password: str) -> tuple[dict[str, Any], str | None]:
        """Returns (status, error key)."""
        client = AudioBrickClient(async_get_clientsession(self.hass), host, password)
        try:
            status = await client.status()
            await client.check_password()
        except AudioBrickAuthError:
            return {}, "invalid_auth"
        except AudioBrickError:
            return {}, "cannot_connect"
        if "version" not in status:
            return {}, "cannot_connect"
        return status, None

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            host = user_input[CONF_HOST].strip()
            status, error = await self._check(host, user_input[CONF_PASSWORD])
            if error:
                errors["base"] = error
            else:
                return await self._create(host, user_input[CONF_PASSWORD], status)
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOST, default=self._host): str,
                    vol.Required(CONF_PASSWORD): str,
                }
            ),
            errors=errors,
        )

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        self._host = discovery_info.host
        # Make sure it really is a Brick (the generic _http._tcp type matches other devices too).
        client = AudioBrickClient(async_get_clientsession(self.hass), self._host, "")
        try:
            status = await client.status()
        except AudioBrickError:
            return self.async_abort(reason="cannot_connect")
        mac = status.get("mac")
        if not mac or "features" not in status:
            return self.async_abort(reason="not_audiobrick")
        await self.async_set_unique_id(mac)
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})
        self.context["title_placeholders"] = {"name": "Audio Brick"}
        return await self.async_step_user()

    async def _create(self, host: str, password: str, status: dict[str, Any]) -> ConfigFlowResult:
        mac = status.get("mac") or host
        await self.async_set_unique_id(mac)
        self._abort_if_unique_id_configured(updates={CONF_HOST: host, CONF_PASSWORD: password})
        return self.async_create_entry(title="Audio Brick", data={CONF_HOST: host, CONF_PASSWORD: password})

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        self._host = entry_data[CONF_HOST]
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            status, error = await self._check(self._host, user_input[CONF_PASSWORD])
            if error:
                errors["base"] = error
            else:
                return self.async_update_reload_and_abort(
                    self._get_reauth_entry(), data_updates={CONF_PASSWORD: user_input[CONF_PASSWORD]}
                )
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=vol.Schema({vol.Required(CONF_PASSWORD): str}),
            errors=errors,
        )
