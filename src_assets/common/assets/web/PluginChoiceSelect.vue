<script>
/**
 * The lists of choices that choices scripts made, shared by every select on the page so a script runs once per
 * option until the list is refreshed. Keyed by `<plugin id>/<option id>`, each entry holds `loading`, `choices`
 * (the last list, or null) and `error`.
 */
import { reactive } from 'vue'
import { apiFetch } from './fetch_utils'

const lists = reactive({});

function listKey(plugin, option) {
  return `${plugin.id}/${option.id}`;
}

/**
 * Run an option's choices script, unless its list was already made or is being made.
 *
 * @param {object} plugin Plugin from `/api/plugins`.
 * @param {object} option One of its options.
 * @param {boolean} refresh Run the script again even when the list was already made.
 */
export async function loadChoices(plugin, option, refresh = false) {
  if (!option.dynamic || plugin.installed === false) {
    return;
  }
  const key = listKey(plugin, option);
  const existing = lists[key];
  if (existing && (existing.loading || (!refresh && existing.choices))) {
    return;
  }

  lists[key] = {loading: true, choices: existing?.choices ?? null, error: null};
  const params = new URLSearchParams({id: plugin.id, option: option.id});
  try {
    const response = await apiFetch(`./api/plugins/choices?${params}`);
    const result = await response.json();
    if (response.ok && result.status === true) {
      lists[key] = {loading: false, choices: result.choices, error: null};
    } else {
      lists[key] = {loading: false, choices: existing?.choices ?? null, error: result.error || String(response.status)};
    }
  } catch (error) {
    console.error('Listing plugin choices failed', error);
    lists[key] = {loading: false, choices: existing?.choices ?? null, error: String(error)};
  }
}

/**
 * Return the choices an option offers: its script's list once made, otherwise the manifest's.
 *
 * @param {object} plugin Plugin from `/api/plugins`.
 * @param {object} option One of its options.
 * @returns {Array<{value: string, label: string, group?: string}>} The choices.
 */
export function choicesOf(plugin, option) {
  return lists[listKey(plugin, option)]?.choices ?? option.choices;
}

/**
 * Describe a value of a choice option by its label, or by the value itself when no choice has it.
 *
 * @param {object} plugin Plugin from `/api/plugins`.
 * @param {object} option One of its options.
 * @param {string} value Value to describe.
 * @returns {string} Label of the value.
 */
export function choiceLabel(plugin, option, value) {
  const choice = choicesOf(plugin, option).find((c) => c.value === value);
  return choice ? choice.label : String(value);
}
</script>

<script setup>
/**
 * A select for a plugin's choice option. When the option has a choices script, the script's list is loaded when
 * the select first shows and can be refreshed; choices with a group are listed under it, after those without.
 * A value no choice has (such as a profile that was deleted) stays selected and is marked as not found.
 */
import { computed, onMounted } from 'vue'
import { RefreshCw } from '@lucide/vue'

const model = defineModel({ type: String, default: '' });
const props = defineProps({
  plugin: {
    type: Object,
    required: true
  },
  option: {
    type: Object,
    required: true
  },
  id: {
    type: String,
    required: true
  },
  disabled: {
    type: Boolean,
    default: false
  }
});

const state = computed(() => lists[listKey(props.plugin, props.option)] ?? null);
const choices = computed(() => choicesOf(props.plugin, props.option));
const ungrouped = computed(() => choices.value.filter((choice) => !choice.group));
const groups = computed(() => {
  const byName = new Map();
  for (const choice of choices.value.filter((c) => c.group)) {
    if (!byName.has(choice.group)) {
      byName.set(choice.group, []);
    }
    byName.get(choice.group).push(choice);
  }
  return [...byName].map(([name, members]) => ({name, choices: members}));
});
const missing = computed(() => model.value !== undefined && model.value !== null &&
  !choices.value.some((choice) => choice.value === model.value));
// Until the script's list is in, a value the manifest doesn't list may still be on it
const listed = computed(() => !props.option.dynamic || Boolean(state.value?.choices));

onMounted(() => loadChoices(props.plugin, props.option));
</script>

<template>
  <div :class="{'input-group': option.dynamic}">
    <select class="form-select" :id="id" :disabled="disabled" :value="model" @change="model = $event.target.value">
      <option v-if="missing" :value="model">
        {{ listed ? $t('plugin_choices.not_found', {value: model}) : model }}
      </option>
      <option v-for="choice in ungrouped" :key="choice.value" :value="choice.value">{{ choice.label }}</option>
      <optgroup v-for="group in groups" :key="group.name" :label="group.name">
        <option v-for="choice in group.choices" :key="choice.value" :value="choice.value">{{ choice.label }}</option>
      </optgroup>
    </select>
    <button v-if="option.dynamic" type="button" class="btn btn-outline-secondary"
            :disabled="disabled || state?.loading || plugin.installed === false"
            :title="$t('plugin_choices.refresh')" :aria-label="$t('plugin_choices.refresh')"
            @click="loadChoices(plugin, option, true)">
      <RefreshCw :size="16" class="icon"></RefreshCw>
    </button>
  </div>
  <div v-if="state?.loading" class="form-text">{{ $t('plugin_choices.loading') }}</div>
  <div v-else-if="state?.error" class="form-text text-danger">{{ $t('plugin_choices.error', {error: state.error}) }}</div>
</template>
