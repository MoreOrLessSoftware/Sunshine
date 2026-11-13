<script setup>
/**
 * Lets an application or a client override a plugin's options, one at a time.
 *
 * `inherited` holds the plugin's own values, and `inheritedKey` is the message describing where the value comes
 * from while an option isn't overridden, given the value as `{value}`.
 *
 * Each option that can be overridden gets an "Override" switch. While it's off, the option shows the value it
 * inherits, greyed out; turning it on starts from that value and lets it be changed. The model holds only the
 * options overridden, as the API expects.
 */
import { computed } from 'vue'
import PluginChoiceSelect, { choiceLabel } from './PluginChoiceSelect.vue'

const model = defineModel({ type: Object, required: true });
const props = defineProps({
  plugin: {
    type: Object,
    required: true
  },
  inherited: {
    type: Object,
    required: true
  },
  inheritedKey: {
    type: String,
    required: true
  },
  idPrefix: {
    type: String,
    required: true
  }
});

const options = computed(() => props.plugin.options.filter((option) => option.overridable));

function controlId(option) {
  return `${props.idPrefix}-${props.plugin.id}-${option.id}`;
}

function isOverridden(option) {
  return Object.hasOwn(model.value, option.id);
}

function valueOf(option) {
  return isOverridden(option) ? model.value[option.id] : props.inherited[option.id];
}

function setOverridden(option, overridden) {
  const next = {...model.value};
  if (overridden) {
    next[option.id] = props.inherited[option.id];
  } else {
    delete next[option.id];
  }
  model.value = next;
}

function setValue(option, value) {
  model.value = {...model.value, [option.id]: value};
}

function numberValue(event) {
  const value = event.target.valueAsNumber;
  return Number.isNaN(value) ? event.target.value : value;
}

/**
 * Describe a value the way the option shows it: a choice by its label, a switch as on or off.
 */
function describe(t, option, value) {
  if (option.type === 'boolean') {
    return t(value ? 'plugin_overrides.on' : 'plugin_overrides.off');
  }
  if (option.type === 'choice') {
    return choiceLabel(props.plugin, option, value);
  }
  return value === '' || value === null || value === undefined ? '""' : String(value);
}
</script>

<template>
  <div v-if="options.length === 0" class="form-text">{{ $t('plugin_overrides.none') }}</div>
  <div v-else class="row g-3">
    <div class="col-12 col-md-6" v-for="option in options" :key="option.id">
      <div class="d-flex align-items-center justify-content-between gap-2 mb-1">
        <label class="form-label mb-0" :for="controlId(option)">{{ option.label }}</label>
        <div class="form-check form-switch mb-0">
          <input class="form-check-input" type="checkbox" role="switch" :id="controlId(option) + '-override'"
                 :checked="isOverridden(option)" @change="setOverridden(option, $event.target.checked)">
          <label class="form-check-label small" :for="controlId(option) + '-override'">{{ $t('plugin_overrides.override') }}</label>
        </div>
      </div>
      <div v-if="option.type === 'boolean'" class="form-check form-switch">
        <input class="form-check-input" type="checkbox" role="switch" :id="controlId(option)"
               :disabled="!isOverridden(option)" :checked="valueOf(option)"
               @change="setValue(option, $event.target.checked)">
        <label class="form-check-label" :for="controlId(option)">{{ $t(valueOf(option) ? 'plugin_overrides.on' : 'plugin_overrides.off') }}</label>
      </div>
      <PluginChoiceSelect v-else-if="option.type === 'choice'" :plugin="plugin" :option="option" :id="controlId(option)"
                          :disabled="!isOverridden(option)" :model-value="valueOf(option)"
                          @update:model-value="setValue(option, $event)" />
      <input v-else-if="option.type === 'number'" type="number" class="form-control" :id="controlId(option)"
             :disabled="!isOverridden(option)" :value="valueOf(option)"
             :min="option.min" :max="option.max" :step="option.step ?? 'any'"
             @input="setValue(option, numberValue($event))">
      <input v-else type="text" class="form-control" :id="controlId(option)"
             :disabled="!isOverridden(option)" :value="valueOf(option)"
             @input="setValue(option, $event.target.value)">
      <div class="form-text" v-if="!isOverridden(option)">
        {{ $t(inheritedKey, {value: describe($t, option, inherited[option.id])}) }}
      </div>
      <div class="form-text" v-else-if="option.description">{{ option.description }}</div>
    </div>
  </div>
</template>
