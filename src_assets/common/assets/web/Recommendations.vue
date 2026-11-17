<template>
  <div class="card my-4" v-if="items.length">
    <div class="card-body">
      <h2 id="recommendations">{{ $t('index.recommendations') }}</h2>
      <p>{{ $t('index.recommendations_desc') }}</p>

      <div class="recommendation-list">
        <article class="recommendation" v-for="item in items" :key="item.id">
          <div class="recommendation-icon" aria-hidden="true">
            <monitor v-if="item.id === 'virtual_display'" :size="21"></monitor>
            <gauge v-else-if="item.id === 'vrr_minimum_fps'" :size="21"></gauge>
            <cpu v-else-if="item.id === 'nvenc_latency' || item.id === 'amf_direct'" :size="21"></cpu>
            <gamepad-2 v-else :size="21"></gamepad-2>
          </div>
          <div class="recommendation-body">
            <h3>{{ $t(`index.rec_${item.id}_title`) }}</h3>
            <p>{{ $t(`index.rec_${item.id}_desc`) }}</p>
            <ul class="recommendation-changes" v-if="item.changes && item.changes.length">
              <li v-for="change in item.changes" :key="change.key">
                {{ $t(`config.${change.key}`) }}: <strong>{{ change.label }}</strong>
              </li>
            </ul>

            <div class="alert alert-success alert-inline mt-2 mb-0" role="status" v-if="results[item.id] && results[item.id].ok">
              <check-circle :size="18" class="icon flex-shrink-0"></check-circle>
              <span>{{ results[item.id].message }}</span>
              <button v-if="results[item.id].restart" class="btn btn-sm btn-success ms-auto" type="button" @click="restartNow">
                <refresh-cw :size="15"></refresh-cw>
                {{ $t('connection.restart_now') }}
              </button>
            </div>
            <div class="alert alert-danger alert-inline mt-2 mb-0" role="alert" v-else-if="results[item.id]">
              <alert-circle :size="18" class="icon flex-shrink-0"></alert-circle>
              <span>{{ results[item.id].message }}</span>
            </div>
            <ul class="recommendation-failures mt-2 mb-0" v-if="results[item.id] && results[item.id].failures">
              <li v-for="failure in results[item.id].failures" :key="failure.id">
                <strong>{{ failure.name }}</strong>: {{ failure.message }}
                <a v-if="failure.url" :href="failure.url" target="_blank" rel="noopener noreferrer">{{ failure.url }}</a>
              </li>
            </ul>

            <div class="recommendation-actions">
              <!-- Done: the result above says so, and only Review and Dismiss stay -->
              <template v-if="!(results[item.id] && results[item.id].ok)">
                <template v-if="item.id === 'virtual_display'">
                  <button class="btn btn-primary" type="button" :disabled="busy !== null" @click="setUpVirtualDisplay(true)">
                    <refresh-cw v-if="busy === 'virtual_display_all'" :size="17" class="driver-release-spinning"></refresh-cw>
                    {{ $t('index.rec_virtual_display_all_apps') }}
                  </button>
                  <button class="btn btn-outline-primary" type="button" :disabled="busy !== null" @click="setUpVirtualDisplay(false)">
                    <refresh-cw v-if="busy === 'virtual_display_app'" :size="17" class="driver-release-spinning"></refresh-cw>
                    {{ $t('index.rec_virtual_display_app_only') }}
                  </button>
                </template>
                <button v-else-if="item.id === 'virtualhid'" class="btn btn-primary" type="button" :disabled="busy !== null" @click="enableVirtualHid">
                  <refresh-cw v-if="busy === 'virtualhid'" :size="17" class="driver-release-spinning"></refresh-cw>
                  {{ $t('index.rec_virtualhid_action') }}
                </button>
                <button v-else-if="item.id === 'xgpad'" class="btn btn-primary" type="button" :disabled="busy !== null" @click="installXgpad">
                  <refresh-cw v-if="busy === 'xgpad'" :size="17" class="driver-release-spinning"></refresh-cw>
                  <download v-else :size="17"></download>
                  {{ $t('index.rec_xgpad_action') }}
                </button>
                <button v-else class="btn btn-primary" type="button" :disabled="busy !== null" @click="applySettings(item)">
                  <refresh-cw v-if="busy === item.id" :size="17" class="driver-release-spinning"></refresh-cw>
                  {{ $t('index.rec_apply_settings') }}
                </button>
              </template>
              <a v-if="item.href" class="btn btn-outline-secondary" :href="item.href">{{ $t('index.rec_review') }}</a>
              <button class="btn btn-link text-body-secondary ms-auto" type="button" @click="dismiss(item.id)">
                {{ $t('index.rec_dismiss') }}
              </button>
            </div>
          </div>
        </article>
      </div>
    </div>
  </div>
</template>

<script>
import {
  AlertCircle,
  CheckCircle,
  Cpu,
  Download,
  Gamepad2,
  Gauge,
  Monitor,
  RefreshCw,
} from '@lucide/vue'
import { apiFetch } from './fetch_utils'
import { notify } from './Notification.vue'
import { confirmDialog } from './ConfirmDialog.vue'
import { restartSunshine } from './server_status'

// Where dismissed recommendations are remembered in the browser
const DISMISSED_KEY = 'index.dismissedRecommendations';

// Fields of the /api/config response that aren't settings, so they're never written back to the config file
const RESPONSE_ONLY_KEYS = ['platform', 'status', 'version', 'display_version', 'update_check', 'active_encoder', 'nvenc_engines', 'virtualhid_driver_installed', 'vigembus_installed'];

// Values the config file accepts for an enabled boolean setting
const TRUTHY_VALUES = ['true', '1', 'enabled', 'enable', 'yes', 'on'];

// The plugins a virtual display stream uses, in the order they're set up. RTSS goes last since it needs RTSS installed.
const VIRTUAL_DISPLAY_PLUGINS = ['virtual-display', 'vsync-toggle', 'rtss-limit'];

// The application the virtual display recommendation adds
const VIRTUAL_DISPLAY_APP = 'Virtual Display';

// Where RTSS can be downloaded, since the RTSS Frame Limit plugin can't install it
const RTSS_URL = 'https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/';

/**
 * Wait for a time.
 *
 * @param {number} ms Milliseconds to wait.
 * @returns {Promise<void>} Resolves once the time has passed.
 */
function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * Whether a config file value turns a boolean setting on.
 *
 * @param {*} value The value from the config file, or undefined when it isn't set.
 * @param {boolean} fallback The setting's default.
 * @returns {boolean} Whether the setting is on.
 */
function isTruthy(value, fallback) {
  if (value === undefined || value === null || value === '') {
    return fallback;
  }
  return TRUTHY_VALUES.includes(`${value}`.toLowerCase().trim());
}

/**
 * Read a JSON response, turning a failed request into an error with the server's message.
 *
 * @param {Response} response The fetch response.
 * @returns {Promise<object>} The parsed body.
 */
async function readJson(response) {
  let body = {};
  try {
    body = await response.json();
  } catch {
    // An empty or non-JSON body
  }
  if (!response.ok || body.status === false) {
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  return body;
}

/**
 * Setting changes that make the most of the home page's hardware and setup, each applied in one click.
 */
export default {
  components: {
    AlertCircle,
    CheckCircle,
    Cpu,
    Download,
    Gamepad2,
    Gauge,
    Monitor,
    RefreshCw,
  },
  props: {
    /** The `/api/config` response, or null while it loads. */
    config: {
      type: Object,
      default: null,
    },
    /** Whether to suggest the xgpad plugin, which installs the xgpad driver, because no gamepad driver is installed. */
    suggestXgpad: {
      type: Boolean,
      default: false,
    },
  },
  emits: ['xgpad-installed', 'virtualhid-enabled', 'restarted'],
  data() {
    return {
      busy: null,
      dismissed: [],
      plugins: null,
      results: {},
    }
  },
  computed: {
    /**
     * The recommendations to show, leaving out dismissed ones and ones already followed.
     *
     * @returns {object[]} Each recommendation's `id`, and for setting changes its `changes` and `href`.
     */
    items() {
      if (!this.config) {
        return [];
      }
      const list = [];
      // A card stays while its action runs, and then to show its result, even once what it recommends is done
      const keepResult = (id) => !!this.results[id] || (typeof this.busy === 'string' && this.busy.startsWith(id));

      // First: without any virtual gamepad driver, controllers from clients don't work at all
      if (this.config.platform === 'windows' && (this.suggestXgpad || keepResult('xgpad'))) {
        list.push({id: 'xgpad', href: './plugins'});
      }

      const virtualDisplay = this.plugins && this.plugins.find((p) => p.id === 'virtual-display');
      if (this.config.platform === 'windows' && virtualDisplay && (!virtualDisplay.active || keepResult('virtual_display'))) {
        list.push({id: 'virtual_display', href: './plugins'});
      }

      const minimumFps = this.config.minimum_fps_target;
      if (`${minimumFps ?? 0}`.trim() !== '-1' || keepResult('vrr_minimum_fps')) {
        list.push({
          id: 'vrr_minimum_fps',
          href: './config#minimum_fps_target',
          changes: [{key: 'minimum_fps_target', value: -1, label: '-1'}],
        });
      }

      if (this.config.active_encoder === 'nvenc') {
        const changes = this.nvencChanges();
        if (changes.length || keepResult('nvenc_latency')) {
          list.push({id: 'nvenc_latency', href: './config#nvenc_preset', changes});
        }
      }

      if (this.config.active_encoder === 'amdvce') {
        const changes = this.amfChanges();
        if (changes.length || keepResult('amf_direct')) {
          list.push({id: 'amf_direct', href: './config#amd_direct', changes});
        }
      }

      const virtualhid = this.plugins && this.plugins.find((p) => p.id === 'virtual-hid');
      if (this.config.platform === 'windows' && this.config.virtualhid_driver_installed && virtualhid && (!virtualhid.active || keepResult('virtualhid'))) {
        list.push({id: 'virtualhid', href: './plugins'});
      }


      return list.filter((item) => !this.dismissed.includes(item.id));
    },
  },
  created() {
    try {
      const saved = JSON.parse(window.localStorage.getItem(DISMISSED_KEY) || '[]');
      this.dismissed = Array.isArray(saved) ? saved : [];
    } catch {
      // Everything shows again without storage
    }
    this.loadPlugins();
  },
  methods: {
    /**
     * Load the plugin list, which says whether the virtual display plugin is active.
     *
     * @returns {Promise<object[]>} The plugins, or an empty list when they can't be loaded.
     */
    async loadPlugins() {
      try {
        const body = await readJson(await fetch('./api/plugins'));
        this.plugins = body.plugins || [];
      } catch (e) {
        console.error('Failed to load plugins:', e);
        this.plugins = this.plugins || [];
      }
      return this.plugins;
    },

    /**
     * The NVENC settings that differ from the lowest-latency ones.
     *
     * @returns {object[]} Each setting's config `key`, the `value` to save and a `label` to show.
     */
    nvencChanges() {
      const c = this.config;
      const t = (key) => this.$t(key);
      const changes = [];
      if (`${c.nvenc_preset ?? 1}` !== '1') {
        changes.push({key: 'nvenc_preset', value: 1, label: `P1 ${t('config.nvenc_preset_1')}`});
      }
      // Sending frames while they encode turns off split-frame encoding, which a GPU with several NVENC units
      // gains more from, so it's only recommended for a GPU known to have one
      if (Number(c.nvenc_engines) === 1 && !isTruthy(c.nvenc_subframe, false)) {
        changes.push({key: 'nvenc_subframe', value: 'enabled', label: t('_common.enabled')});
      }
      if ((c.nvenc_twopass ?? 'quarter_res') !== 'disabled') {
        changes.push({key: 'nvenc_twopass', value: 'disabled', label: t('config.nvenc_twopass_disabled')});
      }
      if (isTruthy(c.nvenc_spatial_aq, false)) {
        changes.push({key: 'nvenc_spatial_aq', value: 'disabled', label: t('_common.disabled')});
      }
      if ((c.nvenc_split_encode ?? 'driver_decides') !== 'driver_decides') {
        changes.push({key: 'nvenc_split_encode', value: 'driver_decides', label: t('config.nvenc_split_encode_driver_decides_def')});
      }
      return changes;
    },

    /**
     * The AMD AMF settings that differ from AMF Direct with each AV1 and HEVC frame encoded in 4 parts, which
     * AMF hands over as each finishes.
     *
     * @returns {object[]} Each setting's config `key`, the `value` to save and a `label` to show.
     */
    amfChanges() {
      const c = this.config;
      const changes = [];
      if (!isTruthy(c.amd_direct, false)) {
        changes.push({key: 'amd_direct', value: 'enabled', label: this.$t('_common.enabled')});
      }
      if (`${c.amd_av1_tiles ?? 1}`.trim() !== '4') {
        changes.push({key: 'amd_av1_tiles', value: 4, label: '4'});
      }
      if (`${c.amd_hevc_slices ?? 1}`.trim() !== '4') {
        changes.push({key: 'amd_hevc_slices', value: 4, label: '4'});
      }
      return changes;
    },

    /**
     * Hide a recommendation in this browser.
     *
     * @param {string} id The recommendation's id.
     */
    dismiss(id) {
      this.dismissed = [...this.dismissed, id];
      try {
        window.localStorage.setItem(DISMISSED_KEY, JSON.stringify(this.dismissed));
      } catch {
        // It shows again on the next visit
      }
    },

    /**
     * Save a recommendation's setting changes and restart Sunshine so they take effect.
     *
     * @param {object} item The recommendation, with its `changes`.
     */
    async applySettings(item) {
      if (!item.changes.length) {
        return;
      }
      const confirmed = await confirmDialog({
        title: this.$t(`index.rec_${item.id}_title`),
        message: this.$t('index.rec_apply_confirm'),
        confirmLabel: this.$t('index.rec_apply_settings'),
      });
      if (!confirmed) {
        return;
      }
      this.busy = item.id;
      try {
        // The config file is written as a whole, so start from what's in it now
        const current = await readJson(await fetch('./api/config'));
        const settings = {...current};
        RESPONSE_ONLY_KEYS.forEach((key) => delete settings[key]);
        item.changes.forEach((change) => {
          settings[change.key] = change.value;
        });

        await readJson(await apiFetch('./api/config', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(settings),
        }));
        // Shows the restart until Sunshine is back, then the home page loads the settings it now uses
        const restarted = await restartSunshine();
        this.results = {...this.results, [item.id]: {ok: true, message: this.$t(restarted ? 'index.rec_applied_restarted' : 'index.rec_applied')}};
        this.$emit('restarted');
      } catch (e) {
        console.error('Failed to apply recommended settings:', e);
        this.results = {...this.results, [item.id]: {ok: false, message: this.$t('index.rec_apply_failed', {error: e.message})}};
      } finally {
        this.busy = null;
      }
    },

    /**
     * Install, or turn on, a plugin, waiting for its install script to finish.
     *
     * @param {string} id The plugin's id.
     * @returns {Promise<string|null>} Why it failed, or null when the plugin is active.
     */
    async activatePlugin(id) {
      const plugin = this.plugins.find((p) => p.id === id);
      if (!plugin) {
        return this.$t('index.rec_plugin_missing');
      }
      if (plugin.installed && !plugin.needs_reinstall) {
        if (!plugin.active) {
          await readJson(await apiFetch('./api/plugins/config', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({id, active: true}),
          }));
        }
        return null;
      }

      await readJson(await apiFetch('./api/plugins/install', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({id, activate: true}),
      }));

      // Install scripts can download drivers, so allow them several minutes
      for (let attempt = 0; attempt < 600; attempt++) {
        await sleep(1000);
        const current = (await this.loadPlugins()).find((p) => p.id === id);
        const task = current && current.task;
        if (task && task.action === 'install' && task.status === 'running') {
          continue;
        }
        if (task && task.action === 'install' && task.status === 'failed') {
          return this.$t('index.rec_plugin_install_failed', {code: task.exit_code});
        }
        return null;
      }
      return this.$t('index.rec_plugin_install_timeout');
    },

    /**
     * Turn on the Virtual HID Driver plugin, which reports driver updates on the home page.
     */
    async enableVirtualHid() {
      this.busy = 'virtualhid';
      try {
        const error = await this.activatePlugin('virtual-hid');
        this.results = {
          ...this.results,
          virtualhid: error ? {ok: false, message: error} : {ok: true, message: this.$t('index.rec_virtualhid_done')},
        };
        if (!error) {
          this.$emit('virtualhid-enabled');
        }
      } catch (e) {
        console.error('Failed to turn on the Virtual HID Driver plugin:', e);
        this.results = {...this.results, virtualhid: {ok: false, message: e.message}};
      } finally {
        this.busy = null;
        await this.loadPlugins();
      }
    },

    /**
     * Turn on virtual displays: set up the virtual display, V-Sync and RTSS plugins and add a Virtual Display
     * application. Plugins run for every application, so when `allApps` is false every other application opts out
     * of them, leaving virtual displays to the new application.
     *
     * @param {boolean} allApps Whether every application streams on a virtual display.
     */
    async setUpVirtualDisplay(allApps) {
      const confirmKey = allApps ? 'index.rec_virtual_display_all_apps_confirm' : 'index.rec_virtual_display_app_only_confirm';
      const confirmed = await confirmDialog({
        title: this.$t('index.rec_virtual_display_title'),
        message: this.$t(confirmKey),
        confirmLabel: this.$t(allApps ? 'index.rec_virtual_display_all_apps' : 'index.rec_virtual_display_app_only'),
      });
      if (!confirmed) {
        return;
      }

      this.busy = allApps ? 'virtual_display_all' : 'virtual_display_app';
      const failures = [];
      try {
        await this.loadPlugins();
        for (const id of VIRTUAL_DISPLAY_PLUGINS) {
          const plugin = this.plugins.find((p) => p.id === id);
          let error;
          try {
            error = await this.activatePlugin(id);
          } catch (e) {
            error = e.message;
          }
          if (error) {
            failures.push({
              id,
              name: plugin ? plugin.name : id,
              message: id === 'rtss-limit' ? this.$t('index.rec_rtss_required') : error,
              url: id === 'rtss-limit' ? RTSS_URL : null,
            });
          }
        }

        const apps = (await readJson(await fetch('./api/apps'))).apps || [];
        if (!apps.some((app) => app.name === VIRTUAL_DISPLAY_APP)) {
          await readJson(await apiFetch('./api/apps', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({name: VIRTUAL_DISPLAY_APP, index: -1, 'image-path': 'virtual_display.png'}),
          }));
        }

        // Only one plugin sets up the display. Activating the virtual display turned it off for the applications
        // another one serves, so where it should be used, that other plugin is turned off instead.
        await this.loadPlugins();
        const virtualDisplay = this.plugins.find((p) => p.id === 'virtual-display');
        const autoExcluded = (virtualDisplay && virtualDisplay.display_exclusions) || [];
        const otherDisplays = this.plugins.filter((p) => p.manages_display && p.active && p.id !== 'virtual-display').map((p) => p.id);

        // The apps are re-read since adding one changes their order
        const sorted = (await readJson(await fetch('./api/apps'))).apps || [];
        const entries = [];
        sorted.forEach((app, index) => {
          const saved = app['exclude-plugins'] || [];
          let excluded;
          if (app.name === VIRTUAL_DISPLAY_APP) {
            excluded = [...new Set([...saved.filter((id) => !VIRTUAL_DISPLAY_PLUGINS.includes(id)), ...otherDisplays])];
          } else if (!allApps) {
            excluded = [...new Set([...saved, ...VIRTUAL_DISPLAY_PLUGINS])];
          } else if (autoExcluded.includes(app.name)) {
            excluded = [...new Set([...saved.filter((id) => id !== 'virtual-display'), ...otherDisplays])];
          } else {
            return;
          }
          if (excluded.length !== saved.length || excluded.some((id) => !saved.includes(id))) {
            entries.push({index, name: app.name, 'exclude-plugins': excluded});
          }
        });
        if (entries.length) {
          await readJson(await apiFetch('./api/apps/plugins', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({apps: entries}),
          }));
        }

        const doneKey = allApps ? 'index.rec_virtual_display_done_all_apps' : 'index.rec_virtual_display_done_app_only';
        this.results = {
          ...this.results,
          virtual_display: failures.length ?
            {ok: false, message: this.$t('index.rec_virtual_display_partial'), failures} :
            {ok: true, message: this.$t(doneKey)},
        };
      } catch (e) {
        console.error('Failed to set up virtual displays:', e);
        this.results = {
          ...this.results,
          virtual_display: {ok: false, message: this.$t('index.rec_apply_failed', {error: e.message}), failures},
        };
      } finally {
        this.busy = null;
        await this.loadPlugins();
      }
    },

    /**
     * Restart Sunshine so a change takes effect, such as a driver it looks for when it starts.
     */
    async restartNow() {
      try {
        if (await restartSunshine()) {
          // Done: drop the Restart Now button from the result
          for (const [id, result] of Object.entries(this.results)) {
            if (result.restart) {
              this.results = {...this.results, [id]: {ok: true, message: this.$t('connection.restart_done')}};
            }
          }
          this.$emit('restarted');
        }
      } catch (e) {
        console.error('Failed to restart Sunshine:', e);
        notify.error(e.message);
      }
    },

    /**
     * Install the xgpad plugin, whose install script installs the Xbox Series controller driver.
     */
    async installXgpad() {
      const confirmed = await confirmDialog({
        title: this.$t('index.rec_xgpad_title'),
        message: this.$t('index.rec_xgpad_confirm'),
        confirmLabel: this.$t('index.rec_xgpad_action'),
      });
      if (!confirmed) {
        return;
      }
      this.busy = 'xgpad';
      try {
        await this.loadPlugins();
        const error = await this.activatePlugin('xgpad');
        this.results = {
          ...this.results,
          xgpad: error ?
            {ok: false, message: this.$t('index.rec_apply_failed', {error})} :
            {ok: true, message: this.$t('index.rec_xgpad_done'), restart: true},
        };
        if (!error) {
          this.$emit('xgpad-installed');
        }
      } catch (e) {
        console.error('Failed to install the xgpad plugin:', e);
        this.results = {...this.results, xgpad: {ok: false, message: this.$t('index.rec_apply_failed', {error: e.message})}};
      } finally {
        this.busy = null;
      }
    },
  },
}
</script>
