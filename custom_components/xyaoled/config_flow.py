"""Config flow for the XYAO LED Panel integration."""
from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.components.bluetooth import (
    BluetoothServiceInfoBleak,
    async_discovered_service_info,
)
from homeassistant.config_entries import (
    ConfigEntry,
    ConfigFlow,
    ConfigFlowResult,
    OptionsFlow,
)
from homeassistant.const import CONF_ADDRESS
from homeassistant.core import callback
from homeassistant.helpers.selector import (
    ColorRGBSelector,
    NumberSelector,
    NumberSelectorConfig,
    NumberSelectorMode,
)

from .const import (
    CONF_INIT_HEX,
    CONF_NOTIFY_COLOR,
    CONF_NOTIFY_SPEED,
    DEFAULT_COLOR,
    DEFAULT_SPEED,
    DOMAIN,
)
from .protocol import NAME_PREFIX, validate_init


class XyaoLedConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle discovery and manual setup of a panel."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovery_info: BluetoothServiceInfoBleak | None = None
        self._discovered: dict[str, str] = {}

    async def async_step_bluetooth(
        self, discovery_info: BluetoothServiceInfoBleak
    ) -> ConfigFlowResult:
        await self.async_set_unique_id(discovery_info.address)
        self._abort_if_unique_id_configured()
        self._discovery_info = discovery_info
        self.context["title_placeholders"] = {"name": discovery_info.name}
        return await self.async_step_bluetooth_confirm()

    async def async_step_bluetooth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        assert self._discovery_info is not None
        if user_input is not None:
            return self.async_create_entry(
                title=self._discovery_info.name,
                data={CONF_ADDRESS: self._discovery_info.address},
            )
        self._set_confirm_only()
        return self.async_show_form(
            step_id="bluetooth_confirm",
            description_placeholders={"name": self._discovery_info.name},
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is not None:
            address = user_input[CONF_ADDRESS]
            await self.async_set_unique_id(address, raise_on_progress=False)
            self._abort_if_unique_id_configured()
            return self.async_create_entry(
                title=self._discovered[address], data={CONF_ADDRESS: address}
            )

        current = self._async_current_ids(include_ignore=False)
        for info in async_discovered_service_info(self.hass, connectable=True):
            if info.address in current or not (info.name or "").startswith(NAME_PREFIX):
                continue
            self._discovered[info.address] = info.name
        if not self._discovered:
            return self.async_abort(reason="no_devices_found")

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_ADDRESS): vol.In(
                        {
                            addr: f"{name} ({addr})"
                            for addr, name in self._discovered.items()
                        }
                    )
                }
            ),
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return XyaoLedOptionsFlow()


class XyaoLedOptionsFlow(OptionsFlow):
    """Options: notify/clock defaults and a device-specific init frame."""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is not None:
            init_hex = user_input.get(CONF_INIT_HEX, "").strip().lower().replace(" ", "")
            error = None
            if init_hex:
                try:
                    validate_init(bytes.fromhex(init_hex))
                except ValueError:
                    error = "invalid_hex" if _not_hex(init_hex) else "invalid_init"
            if error:
                return self.async_show_form(
                    step_id="init",
                    data_schema=self._schema({**user_input, CONF_INIT_HEX: init_hex}),
                    errors={CONF_INIT_HEX: error},
                )
            return self.async_create_entry(
                data={
                    CONF_INIT_HEX: init_hex,
                    CONF_NOTIFY_COLOR: list(
                        user_input.get(CONF_NOTIFY_COLOR, list(DEFAULT_COLOR))
                    ),
                    CONF_NOTIFY_SPEED: int(
                        user_input.get(CONF_NOTIFY_SPEED, DEFAULT_SPEED)
                    ),
                }
            )

        opts = self.config_entry.options
        return self.async_show_form(
            step_id="init",
            data_schema=self._schema(
                {
                    CONF_INIT_HEX: opts.get(CONF_INIT_HEX, ""),
                    CONF_NOTIFY_COLOR: opts.get(CONF_NOTIFY_COLOR, list(DEFAULT_COLOR)),
                    CONF_NOTIFY_SPEED: opts.get(CONF_NOTIFY_SPEED, DEFAULT_SPEED),
                }
            ),
        )

    def _schema(self, current: dict[str, Any]) -> vol.Schema:
        return vol.Schema(
            {
                vol.Optional(
                    CONF_NOTIFY_COLOR,
                    default=current.get(CONF_NOTIFY_COLOR, list(DEFAULT_COLOR)),
                ): ColorRGBSelector(),
                vol.Optional(
                    CONF_NOTIFY_SPEED,
                    default=current.get(CONF_NOTIFY_SPEED, DEFAULT_SPEED),
                ): NumberSelector(
                    NumberSelectorConfig(min=1, max=255, mode=NumberSelectorMode.SLIDER)
                ),
                vol.Optional(
                    CONF_INIT_HEX, default=current.get(CONF_INIT_HEX, "")
                ): str,
            }
        )


def _not_hex(s: str) -> bool:
    try:
        bytes.fromhex(s)
        return False
    except ValueError:
        return True
