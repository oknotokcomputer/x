#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 2017 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Transforms and validates cros config from source YAML to target JSON"""

from __future__ import print_function

import argparse
import collections
import copy
import functools
import itertools
import json
import os
import re
import sys

import six


# pylint: disable=wrong-import-position
this_dir = os.path.dirname(__file__)
sys.path.insert(0, this_dir)
# pylint: disable=import-error
import configfs
import identity_table
import libcros_schema


# pylint: enable=import-error


sys.path.pop(0)

CHROMEOS = "chromeos"
CONFIGS = "configs"
DEVICES = "devices"
PRODUCTS = "products"
SKUS = "skus"
CONFIG = "config"
BRAND_ELEMENTS = [
    "brand-code",
    "firmware-signing",
    "wallpaper",
    "regulatory-label",
    "branding",
]
# External stylus is allowed for custom labels
EXTERNAL_STYLUS = "external"
TEMPLATE_PATTERN = re.compile("{{([^}]*)}}")
ALLOWED_CUSTOM_LABEL_FEATURES = {"CloudGamingDevice"}


def MergeDictionaries(primary, overlay):
    """Merges the overlay dictionary onto the primary dictionary.

    If an element doesn't exist, it's added.
    If the element is a list, they are appended to each other.
    Otherwise, the overlay value takes precedent.

    Args:
        primary: Primary dictionary
        overlay: Overlay dictionary
    """
    for overlay_key in overlay.keys():
        overlay_value = overlay[overlay_key]
        if not overlay_key in primary:
            primary[overlay_key] = overlay_value
        elif isinstance(overlay_value, collections.abc.Mapping):
            MergeDictionaries(primary[overlay_key], overlay_value)
        elif isinstance(overlay_value, list):
            primary[overlay_key].extend(overlay_value)
        else:
            primary[overlay_key] = overlay_value


def ParseArgs(argv):
    """Parse the available arguments.

    Invalid arguments or -h cause this function to print a message and exit.

    Args:
        argv: List of string arguments (excluding program name / argv[0])

    Returns:
        argparse.Namespace object containing the attributes.
    """
    parser = argparse.ArgumentParser(
        description="Validates a YAML cros-config and transforms it to JSON"
    )
    parser.add_argument(
        "-s",
        "--schema",
        type=str,
        help="Path to the schema file used to validate the config",
    )
    parser.add_argument(
        "-c",
        "--config",
        type=str,
        help="Path to the YAML config file that will be validated/transformed",
    )
    parser.add_argument(
        "-m",
        "--configs",
        nargs="+",
        type=str,
        help=(
            "Path to the YAML config file(s) that will be "
            "validated/transformed"
        ),
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        help=(
            "Output file that will be generated by the transform"
            "(system file)"
        ),
    )
    parser.add_argument(
        "--configfs-output",
        type=str,
        help=(
            "Path to generated SquashFS filesystem for use in ChromeOS "
            "ConfigFS"
        ),
    )
    parser.add_argument(
        "-f",
        "--filter",
        type=bool,
        default=False,
        help="Filter build specific elements from the output JSON",
    )
    # TODO(b:185470553): this argument is being used to support the
    # Zephyr builders for proof-of-concept devices (devices which
    # normally target a CrOS EC), and can be removed once those builders
    # are no longer needed.
    parser.add_argument(
        "--zephyr-ec-configs-only",
        action="store_true",
        help=(
            "Remove any configuration which does not specify "
            "/firmware/build-targets:zephyr-ec"
        ),
    )
    parser.add_argument(
        "--identity-table-out",
        type=argparse.FileType("wb"),
        help="Output path for identity table",
    )
    return parser.parse_args(argv)


def _SetTemplateVars(template_input, template_vars):
    """Builds a map of template variables by walking the input recursively.

    Args:
        template_input: A mapping object to be walked.
        template_vars: A mapping object built up while walking template_input.
    """
    to_add = {}
    for key, val in template_input.items():
        if isinstance(val, collections.abc.Mapping):
            _SetTemplateVars(val, template_vars)
        elif not isinstance(val, list):
            to_add[key] = val

    # Do this last so all variables from the parent scope win.
    template_vars.update(to_add)


def _GetVarTemplateValue(val, template_input, template_vars):
    """Applies the templating scheme to a single value.

    Args:
        val: The single val to evaluate.
        template_input: Input that will be updated based on the templating
                        schema.
        template_vars: A mapping of all the variables values available.

    Returns:
        The variable value with templating applied.
    """
    for template_var in TEMPLATE_PATTERN.findall(val):
        replace_string = "{{%s}}" % template_var
        if template_var not in template_vars:
            formatted_vars = json.dumps(template_vars, sort_keys=True, indent=2)
            formatted_input = json.dumps(
                template_input, sort_keys=True, indent=2
            )
            error_vals = (template_var, val, formatted_input, formatted_vars)
            raise ValidationError(
                "Referenced template variable '%s' doesn't "
                "exist string '%s'.\nInput:\n %s\nVariables:\n%s" % error_vals
            )
        var_value = template_vars[template_var]

        # This is an ugly side effect of templating with primitive values.
        # The template is a string, but the target value needs to be int.
        # This is sort of a hack for now, but if the problem gets worse, we
        # can come up with a more scaleable solution.
        #
        # Guessing this problem won't continue though beyond the use of
        # 'sku-id' since that tends to be the only strongly typed value due to
        # its use for identity detection.
        is_int = isinstance(var_value, int)
        if is_int:
            var_value = str(var_value)

        # If the caller only had one value and it was a template variable that
        # was an int, assume the caller wanted the string to be an int.
        if is_int and val == replace_string:
            val = template_vars[template_var]
        else:
            val = val.replace(replace_string, var_value)
    return val


def _ApplyTemplateVars(template_input, template_vars):
    """Evals the input, applies the templating schema using the provided vars.

    Args:
        template_input: Input that will be updated based on the templating
                        schema.
        template_vars: A mapping of all the variables values available.
    """
    maps = []
    lists = []
    for key in template_input.keys():
        val = template_input[key]
        if isinstance(val, collections.abc.Mapping):
            maps.append(val)
        elif isinstance(val, list):
            index = 0
            for list_val in val:
                if isinstance(list_val, collections.abc.Mapping):
                    lists.append(list_val)
                elif isinstance(list_val, six.string_types):
                    val[index] = _GetVarTemplateValue(
                        list_val, template_input, template_vars
                    )
                index += 1
        elif isinstance(val, six.string_types):
            template_input[key] = _GetVarTemplateValue(
                val, template_input, template_vars
            )

    # Do this last so all variables from the parent are in scope first.
    for value in maps:
        _ApplyTemplateVars(value, template_vars)

    # Object lists need their variables put in scope on a per list item basis
    for value in lists:
        list_item_vars = copy.deepcopy(template_vars)
        _SetTemplateVars(value, list_item_vars)
        while _HasTemplateVariables(list_item_vars):
            _ApplyTemplateVars(list_item_vars, list_item_vars)
        _ApplyTemplateVars(value, list_item_vars)


def _DeleteTemplateOnlyVars(template_input):
    """Deletes all variables starting with $

    Args:
        template_input: Input that will be updated based on the templating
                        schema.
    """
    to_delete = []
    for key in template_input.keys():
        val = template_input[key]
        if isinstance(val, collections.abc.Mapping):
            _DeleteTemplateOnlyVars(val)
        elif isinstance(val, list):
            for v in val:
                if isinstance(v, collections.abc.Mapping):
                    _DeleteTemplateOnlyVars(v)
        elif key.startswith("$"):
            to_delete.append(key)

    for key in to_delete:
        del template_input[key]


def _HasTemplateVariables(template_vars):
    """Checks if there are any unevaluated template variables.

    Args:
        template_vars: A mapping of all the variables values available.

    Returns:
        True if they are still unevaluated template variables.
    """
    for val in template_vars.values():
        if isinstance(val, six.string_types) and TEMPLATE_PATTERN.findall(val):
            return True


def _TransformDeprecatedConfigs(config):
    """Rework any deprecated configs prior to schema validation.

    This function allows you to translate configs at the time that the
    YAML file is read.  The purpose of this is if a config name is
    changing, and you don't want to go Cq-Depend on all the overlays,
    you can temporarily add the config translation to this function, and
    then go adjust all the overlays.  When you're finished up with the
    overlays, you can go and remove the translation from this function.

    Args:
        config: A dictionary representing the device config, after any
        merges and imports have been completed, but before schema
        validation is performed.  This config will be modified in place.
    """
    firmware = config.get("firmware", {})
    build_targets = firmware.get("build-targets", {})
    if "ec_extras" in build_targets and "ec-extras" not in build_targets:
        build_targets["ec-extras"] = build_targets.pop(
            "ec_extras"
        )  # b/218973795

    identity = config.get("identity", {})
    if "whitelabel-tag" in identity and "custom-label-tag" not in identity:
        identity["custom-label-tag"] = identity.pop("whitelabel-tag")


def TransformConfig(config, model_filter_regex=None):
    """Transforms the source config (YAML) to the target system format (JSON)

    Applies consistent transforms to covert a source YAML configuration into
    JSON output that will be used on the system by cros_config.

    Args:
        config: Config that will be transformed.
        model_filter_regex: Only returns configs that match the filter

    Returns:
        Resulting JSON output from the transform.
    """
    config_yaml = libcros_schema.LoadYaml(config)
    configs = []
    if DEVICES in config_yaml[CHROMEOS]:
        for device in config_yaml[CHROMEOS][DEVICES]:
            template_vars = {}
            for product in device.get(PRODUCTS, [{}]):
                for sku in device[SKUS]:
                    # Template variables scope is config, then device,
                    # then product.
                    # This allows shared configs to define defaults using
                    # anchors, which can then be easily overridden by the
                    # product/device scope.
                    _SetTemplateVars(sku, template_vars)
                    _SetTemplateVars(device, template_vars)
                    _SetTemplateVars(product, template_vars)
                    while _HasTemplateVariables(template_vars):
                        _ApplyTemplateVars(template_vars, template_vars)
                    sku_clone = copy.deepcopy(sku)
                    _ApplyTemplateVars(sku_clone, template_vars)
                    config = sku_clone[CONFIG]
                    _DeleteTemplateOnlyVars(config)
                    configs.append(config)
    else:
        configs = config_yaml[CHROMEOS][CONFIGS]

    if model_filter_regex:
        matcher = re.compile(model_filter_regex)
        configs = [
            config for config in configs if matcher.match(config["name"])
        ]

    for device_config in configs:
        _TransformDeprecatedConfigs(device_config)

    # Drop everything except for configs since they were just used as shared
    # config in the source yaml.
    json_config = {
        CHROMEOS: {
            CONFIGS: configs,
        },
    }

    return libcros_schema.FormatJson(json_config)


def _GenerateInferredAshSwitches(device_config):
    """Generate runtime-packed ash switches into a single device config.

    Chrome switches are packed into /ui:serialized-ash-switches in the
    resultant runtime-only configuration, as a string of null-terminated
    strings.

    Args:
        device_config: transformed configuration for a single device.

    Returns:
        Config for a single device with /ui:serialized-ash-switches added.
    """
    ui_config = device_config.get("ui", {})
    ash_switches = set()
    ash_switches |= set(ui_config.get("extra-ash-flags", []))
    ash_enabled_features = set(ui_config.get("ash-enabled-features", []))
    ash_disabled_features = set(ui_config.get("ash-disabled-features", []))

    extra_web_apps_dir = ui_config.get("apps", {}).get("extra-web-apps-dir")
    if extra_web_apps_dir:
        ash_switches.add("--extra-web-apps-dir=%s" % extra_web_apps_dir)

    demo_mode_config = device_config.get("demo-mode", {})
    for ext_type in ("highlights", "screensaver"):
        ext_id = demo_mode_config.get("%s-extension-id" % ext_type)
        if ext_id:
            ash_switches.add("--demo-mode-%s-extension=%s" % (ext_type, ext_id))

    has_numpad = device_config.get("keyboard", {}).get("numpad")
    if has_numpad:
        ash_switches.add("--has-number-pad")

    display_properties = device_config.get("displays")
    if display_properties:
        ash_switches.add(
            "--display-properties=%s" % json.dumps(display_properties)
        )

    defer_external_display_timeout = device_config.get(
        "power", {}).get("defer-external-display-timeout")
    if defer_external_display_timeout:
        ash_switches.add(
            "--defer-external-display-timeout=%s"
            % defer_external_display_timeout)

    if ash_enabled_features:
        ash_switches.add(
            "--enable-features=%s"
            % ",".join(
                sorted(feature for feature in ash_enabled_features if feature)
            )
        )
    if ash_disabled_features:
        ash_switches.add(
            "--disable-features=%s"
            % ",".join(
                sorted(feature for feature in ash_disabled_features if feature)
            )
        )

    if not ash_switches:
        return device_config

    serialized_ash_switches = ""
    for flag in sorted(ash_switches):
        serialized_ash_switches += "%s\0" % flag

    device_config = copy.deepcopy(device_config)
    device_config.setdefault("ui", {})
    device_config["ui"]["serialized-ash-switches"] = serialized_ash_switches
    return device_config


def _GenerateInferredElements(json_config):
    """Generates runtime-only elements.

    These are elements which can be inferred from a config containing
    build-only elements which only appear at runtime.  For example, this
    can be used to generate an application-specific representation of an
    otherwise abstracted configuration.

    Args:
        json_config: transformed config dictionary to use.

    Returns:
        Config dictionary, with inferred elements potentially added.
    """
    configs = []
    for config in json_config[CHROMEOS][CONFIGS]:
        ui_elements = config.get("ui", {})

        identity = config.get("identity", {})
        custom_label_tag = identity.get("custom-label-tag")

        # Old name for custom-label-tag was whitelabel-tag.  Allow this
        # name as an alternative until we validate no clients use it.
        if custom_label_tag:
            identity["whitelabel-tag"] = custom_label_tag

        if "help-content-id" not in ui_elements:
            customization_id = identity.get("customization-id")
            model_name = config.get("name")
            ui_elements["help-content-id"] = (
                customization_id or custom_label_tag or model_name
            )
        config["ui"] = ui_elements
        config = _GenerateInferredAshSwitches(config)
        configs.append(config)
    return {CHROMEOS: {CONFIGS: configs}}


def FilterBuildElements(config, build_only_elements):
    """Removes build only elements from the schema.

    Removes build only elements from the schema in preparation for the
    platform, and generates any runtime-only inferred elements.

    Args:
        config: Config (transformed) that will be filtered
        build_only_elements: List of strings of paths of fields to be filtered
    """
    json_config = json.loads(config)
    json_config = _GenerateInferredElements(json_config)
    for device_config in json_config[CHROMEOS][CONFIGS]:
        _FilterBuildElements(device_config, "", build_only_elements)

    return libcros_schema.FormatJson(json_config)


def _FilterBuildElements(config, path, build_only_elements):
    """Recursively checks and removes build only elements.

    Args:
        config: Dict that will be checked.
        path: Path of elements to filter.
        build_only_elements: List of strings of paths of fields to be filtered
    """
    to_delete = []
    for key in config:
        full_path = "%s/%s" % (path, key)
        if full_path in build_only_elements:
            to_delete.append(key)
        elif isinstance(config[key], dict):
            _FilterBuildElements(config[key], full_path, build_only_elements)
    for key in to_delete:
        config.pop(key)


def FilterNonZephyrDevices(config):
    """Remove any devices which do not specify a Zephyr EC build target.

    Args:
        config: JSON-serialized configuration.

    Returns:
        JSON-serialized configuration, potentially with some configs gone.
    """
    json_config = json.loads(config)
    new_device_configs = []
    for device_config in json_config[CHROMEOS][CONFIGS]:
        build_targets = device_config.get("firmware", {}).get(
            "build-targets", {}
        )
        if "zephyr-ec" in build_targets:
            new_device_configs.append(device_config)
    return libcros_schema.FormatJson({CHROMEOS: {CONFIGS: new_device_configs}})


def GenerateFridMatches(json_config):
    """Generate covering FRID matches.

    For models with configs where FRID values are not uniform, generate identity
    configs with all FRID values for each rest of the identity object for that
    model.

    Args:
        json_config: JSON config dictionary

    Returns:
        A new JSON config dictionary with per-model FRID coverage filled in.
    """
    # Maps model name to all possible FRIDs for this model.
    model_frid_matches = collections.defaultdict(set)
    for config in json_config[CHROMEOS][CONFIGS]:
        name = config["name"]
        identity = config.get("identity", {})
        model_frid_matches[name].add(identity.get("frid"))
    sorted_model_frid_matches = {
        k: sorted(v) for k, v in model_frid_matches.items()
    }

    new_configs = []
    model_identity_matches = collections.defaultdict(set)
    for config in json_config[CHROMEOS][CONFIGS]:
        name = config["name"]
        matches = sorted_model_frid_matches[name]
        if len(matches) == 1:
            new_configs.append(config)
            continue

        template_identity = config.get("identity", {})

        for frid in matches:
            new_identity = dict(template_identity, frid=frid)
            hashable_new_identity = tuple(sorted(new_identity.items()))
            if hashable_new_identity in model_identity_matches[name]:
                continue
            model_identity_matches[name].add(hashable_new_identity)
            new_config = dict(config, identity=new_identity)
            new_configs.append(new_config)

    return {CHROMEOS: {CONFIGS: new_configs}}


@functools.lru_cache()
def GetValidSchemaProperties(
    schema=os.path.join(this_dir, "cros_config_schema.yaml")
):
    """Returns all valid properties from the given schema

    Iterates over the config payload for devices and returns the list of
    valid properties that could potentially be returned from
    cros_config_host or cros_config

    Args:
        schema: Source schema that contains the properties.
    """
    schema_yaml = ReadSchema(schema)
    root_path = "properties/chromeos/properties/configs/items/properties"
    schema_node = libcros_schema.LoadYaml(schema_yaml)
    for element in root_path.split("/"):
        schema_node = schema_node[element]

    result = {}
    _GetValidSchemaProperties(schema_node, [], result)
    return result


def _GetValidSchemaProperties(schema_node, path, result):
    """Recursively finds the valid properties for a given node

    Args:
        schema_node: Single node from the schema
        path: Running path that a given node maps to
        result: Running collection of results
    """
    full_path = "/%s" % "/".join(path)
    valid_schema_property_types = {"array", "boolean", "integer", "string"}
    for key in schema_node:
        new_path = path + [key]
        node_type = schema_node[key]["type"]

        if node_type == "object":
            if "properties" in schema_node[key]:
                _GetValidSchemaProperties(
                    schema_node[key]["properties"], new_path, result
                )
        elif node_type in valid_schema_property_types:
            all_props = result.get(full_path, [])
            all_props.append(key)
            result[full_path] = all_props


class ValidationError(Exception):
    """Exception raised for a validation error"""


def _IdentityProjection(identity):
    """Returns a semantic projection of an identity dictionary.

    Args:
        identity: An identity dictionary.

    Returns:
        A hashable projection of the identity dictionary that compares equal to
        the value returned by this function for all semantically equivalent,
        with respect to identity matching, identity dictionaries.
    """

    def _FoldValue(value):
        # Values we can get here are integers, strings, or None.  Use
        # .lower() on strings, do nothing to everything else.
        if isinstance(value, str):
            # Consider strings of differing case to be equivalent.
            return value.lower()
        return value

    # The platform-name plays no role in identity matching, so skip it
    # when considering equivalency.
    # TODO(crbug.com/1070692): Move /identity:platform-name to
    # /mosys:platform-name so we can skip this.
    keys = set(identity.keys()) - {"platform-name"}

    return tuple(sorted((k, _FoldValue(identity[k])) for k in keys))


def _IdentityEq(a, b):
    """Equality function for two identity dictionaries.

    Args:
        a: An identity dictionary.
        b: Another identity dictionary.

    Returns:
        True if a is semantically equivalent to b with respect to identity
        matching, False otherwise.
    """
    return _IdentityProjection(a) == _IdentityProjection(b)


def _ValidateUniqueIdentities(json_config):
    """Verifies the identity tuple is globally unique within the config.

    Args:
        json_config: JSON config dictionary
    """
    for config in json_config["chromeos"]["configs"]:
        if "identity" not in config and "name" not in config:
            raise ValidationError(
                "Missing identity for config: %s" % str(config)
            )

    for config_a, config_b in itertools.combinations(
        json_config["chromeos"]["configs"], 2
    ):
        if _IdentityEq(config_a["identity"], config_b["identity"]):
            raise ValidationError(
                "Identities are not unique: %s and %s"
                % (config_a["identity"], config_b["identity"])
            )


def _ValidateCustomLabelBrandChangesOnly(json_config):
    """Verifies that custom label changes are contained to branding information.

    Args:
        json_config: JSON config dictionary
    """
    custom_labels = {}
    for config in json_config["chromeos"]["configs"]:
        if "custom-label-tag" in config.get("identity", {}):
            if (
                "bobba" in config["name"]
            ):  # Remove after crbug.com/1036381 resolved
                continue
            name = "%s - %s" % (
                config["name"],
                config["identity"].get("sku-id", 0),
            )
            config_list = custom_labels.get(name, [])

            config_minus_brand = copy.deepcopy(config)
            config_minus_brand["identity"]["custom-label-tag"] = ""

            for brand_element in BRAND_ELEMENTS:
                config_minus_brand[brand_element] = ""

            hw_props = config_minus_brand.get("hardware-properties", None)
            if hw_props:
                stylus = hw_props.get("stylus-category", "none")
                if stylus == "none" or stylus == EXTERNAL_STYLUS:
                    hw_props.pop("stylus-category", None)

            # Remove /ui:help-content-id
            if "ui" not in config_minus_brand:
                config_minus_brand["ui"] = {}
            config_minus_brand["ui"]["help-content-id"] = ""

            # Trim allowed feature flags from /ui:ash-enabled-features
            if "ash-enabled-features" in config_minus_brand["ui"]:
                config_minus_brand["ui"]["ash-enabled-features"] = sorted(
                    feature
                    for feature in set(
                        config_minus_brand["ui"]["ash-enabled-features"]
                    )
                    if feature and feature not in ALLOWED_CUSTOM_LABEL_FEATURES
                )

            config_minus_brand.get("arc", {}).get("build-properties", {}).pop(
                "marketing-name", None
            )
            config_minus_brand.get("arc", {}).get("build-properties", {}).pop(
                "oem", None
            )
            config_minus_brand.get("identity", {}).pop("frid", None)

            config_list.append(config_minus_brand)
            custom_labels[name] = config_list

    # custom_labels now contains a map by device name with all custom label
    # configs that have had their branding data stripped.
    for device_name, configs in custom_labels.items():
        base_config = configs[0]
        for compare_config in configs[1:]:
            if base_config != compare_config:
                raise ValidationError(
                    "Custom label configs can only change branding attributes "
                    "or use an external stylus for (%s).\n"
                    "However, the device %s differs by other attributes.\n"
                    "Example 1: %s\n"
                    "Example 2: %s"
                    % (
                        device_name,
                        ", ".join(BRAND_ELEMENTS),
                        base_config,
                        compare_config,
                    )
                )


def _ValidateHardwarePropertiesAreValidType(json_config):
    """Checks that all fields under hardware-properties are boolean

       Ensures that no key is added to hardware-properties that has a
       non-boolean value, because non-boolean values are unsupported by the
       hardware-properties codegen.

    Args:
        json_config: JSON config dictionary
    """
    for config in json_config["chromeos"]["configs"]:
        hardware_properties = config.get("hardware-properties", None)
        if hardware_properties:
            for key, value in hardware_properties.items():
                if not isinstance(value, (bool, six.string_types)):
                    raise ValidationError(
                        f"All configs under hardware-properties must be "
                        f"boolean or an enum\n"
                        f"However, key '{key}' has value '{value}'."
                    )


def _ValidateConsistentFingerprintFirmwareROVersion(configs):
    """Validate all /fingerprint:ro-version entries.

    A given Chrome OS board can only have a single RO version for a given FPMCU
    board. See
    http://go/cros-fingerprint-firmware-branching-and-signing#single-ro-per-mcu for details.  # pylint: disable=line-too-long

    Args:
        configs: The transformed config to be validated.
    """
    expected_ro_version = collections.defaultdict(set)
    for device in configs[CHROMEOS][CONFIGS]:
        fingerprint = device.get("fingerprint")
        if fingerprint is None:
            return

        fpmcu = fingerprint.get("board")
        ro_version = fingerprint.get("ro-version")
        expected_ro_version[fpmcu].add(ro_version)

    for versions in expected_ro_version.values():
        if len(versions) != 1:
            raise ValidationError(
                "You may not use different fingerprint firmware RO versions "
                "on the same board: %s" % expected_ro_version
            )


def _ValidateFeatureDeviceTypeIdentities(json_config):
    """Validates that each feature-device-type identity matches.

    Each identity with a present feature-device-type value must have a
    corresponding config with a feature-device-type value absent.

    Args:
        json_config: The transformed config to be validated.
    """
    on_identities = {}
    feature_device_type_absent_identities = set()
    for config in json_config["chromeos"]["configs"]:
        identity = config.get("identity", {})
        feature_device_type = identity.get("feature-device-type")
        rest_of_identity = _IdentityProjection(
            {k: v for k, v in identity.items() if k != "feature-device-type"}
        )
        if feature_device_type is not None:
            on_identities[rest_of_identity] = identity
        else:
            feature_device_type_absent_identities.add(rest_of_identity)

    for rest_of_identity, identity in on_identities.items():
        if rest_of_identity not in feature_device_type_absent_identities:
            raise ValidationError(
                "Any config enabling feature-device-type must have a "
                "corresponding feature-device-type absent identity.\n"
                f"Example: {identity}"
            )


def ValidateConfig(config):
    """Validates a transformed cros config for general business rules.

    Performs name uniqueness checks and any other validation that can't be
    easily performed using the schema.

    Args:
        config: Config (transformed) that will be verified.
    """
    json_config = json.loads(config)
    _ValidateUniqueIdentities(json_config)
    _ValidateCustomLabelBrandChangesOnly(json_config)
    _ValidateHardwarePropertiesAreValidType(json_config)
    _ValidateConsistentFingerprintFirmwareROVersion(json_config)
    _ValidateFeatureDeviceTypeIdentities(json_config)


def MergeConfigs(configs):
    """Evaluates and merges all config files into a single configuration.

    Args:
        configs: List of source config files that will be transformed/merged.

    Returns:
        Final merged JSON result.
    """
    json_files = []
    for yaml_file in configs:
        yaml_with_imports = libcros_schema.ApplyImports(yaml_file)
        json_transformed_file = TransformConfig(yaml_with_imports)
        json_files.append(json.loads(json_transformed_file))

    result_json = json_files[0]
    for overlay_json in json_files[1:]:
        for to_merge_config in overlay_json["chromeos"]["configs"]:
            to_merge_identity = to_merge_config.get("identity", {})
            to_merge_name = to_merge_config.get("name", "")
            matched = False
            # Find all existing configs where there is a full/partial identity
            # match or name match and merge that config into the source.
            # If there are no matches, then append the config.
            for source_config in result_json["chromeos"]["configs"]:
                identity_match = False
                if to_merge_identity:
                    source_identity = source_config["identity"]

                    # If we are missing anything from the source identity, copy
                    # it into to_merge_identity before doing the comparison, as
                    # missing attributes in the to_merge_identity should be
                    # treated as matched.
                    to_merge_identity_extended = to_merge_identity.copy()
                    for key, value in source_identity.items():
                        if key not in to_merge_identity_extended:
                            to_merge_identity_extended[key] = value

                    identity_match = _IdentityEq(
                        source_identity, to_merge_identity_extended
                    )
                elif to_merge_name:
                    identity_match = to_merge_name == source_config.get(
                        "name", ""
                    )

                if identity_match:
                    MergeDictionaries(source_config, to_merge_config)
                    matched = True

            if not matched:
                result_json["chromeos"]["configs"].append(to_merge_config)

    return libcros_schema.FormatJson(GenerateFridMatches(result_json))


def ReadSchema(schema=None):
    """Reads the schema file and evaluates all import statements.

    Args:
        schema: Schema file used to verify the config.

    Returns:
        Schema contents with imports evaluated.
    """
    if not schema:
        schema = os.path.join(this_dir, "cros_config_schema.yaml")
    return libcros_schema.ApplyImports(schema)


def Main(
    schema,
    config,
    output,
    filter_build_details=False,
    configfs_output=None,
    configs=None,
    zephyr_ec_configs_only=False,
    identity_table_out=None,
):
    """Transforms and validates a cros config file for use on the system

    Applies consistent transforms to covert a source YAML configuration into
    a JSON file that will be used on the system by cros_config.

    Verifies that the file complies with the schema verification rules and
    performs additional verification checks for config consistency.

    Args:
        schema: Schema file used to verify the config.
        config: Source config file that will be transformed/verified.
        output: Output file that will be generated by the transform.
        filter_build_details: Whether build only details should be filtered or
                              not.
        configfs_output: Output path to generated SquashFS for ConfigFS.
        configs: List of source config files that will be transformed/verified.
        zephyr_ec_configs_only: True if device configs which do not
        contain /firmware/build-targets:zephyr-ec should be removed.
        identity_table_out: Output file for crosid identity table.
    """
    # TODO(shapiroc): Remove this once we no longer need backwards compatibility
    # for single config parameters.
    if config:
        configs = [config]

    full_json_transform = MergeConfigs(configs)
    json_transform = full_json_transform

    schema_contents = ReadSchema(schema)
    libcros_schema.ValidateConfigSchema(schema_contents, json_transform)
    ValidateConfig(json_transform)
    schema_attrs = libcros_schema.GetSchemaPropertyAttrs(
        libcros_schema.LoadYaml(schema_contents)
    )

    if zephyr_ec_configs_only:
        json_transform = FilterNonZephyrDevices(json_transform)
    if filter_build_details:
        build_only_elements = []
        for path in schema_attrs:
            if schema_attrs[path].build_only_element:
                build_only_elements.append(path)
        json_transform = FilterBuildElements(
            json_transform, build_only_elements
        )
    if output:
        with open(output, "w", encoding="utf-8") as output_stream:
            # Using print function adds proper trailing newline.
            print(json_transform, file=output_stream)
    else:
        print(json_transform)
    if configfs_output:
        configfs.GenerateConfigFSData(
            json.loads(json_transform), configfs_output
        )
    if identity_table_out:
        identity_table.WriteIdentityStruct(
            json.loads(json_transform), identity_table_out
        )


# The distutils generated command line wrappers will not pass us argv.
def main(argv=None):
    """Main program which parses args and runs

    Args:
        argv: List of command line arguments, if None uses sys.argv.
    """
    if argv is None:
        argv = sys.argv[1:]
    opts = ParseArgs(argv)
    Main(
        opts.schema,
        opts.config,
        opts.output,
        opts.filter,
        opts.configfs_output,
        opts.configs,
        opts.zephyr_ec_configs_only,
        opts.identity_table_out,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
