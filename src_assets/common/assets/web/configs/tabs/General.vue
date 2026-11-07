<script setup>
import Checkbox from '../../Checkbox.vue'
import { ref } from 'vue'
import {
  Play,
  Plus,
  Shield,
  Trash2,
  Undo,
} from '@lucide/vue'

const props = defineProps({
  platform: String,
  config: Object
})
const config = ref(props.config)

// Commands saved before prep command types existed run on app start/stop
for (const c of config.value.global_prep_cmd) {
  if (c.type === undefined) {
    c.type = "app";
  }
}

function addCmd() {
  let template = {
    type: "app",
    do: "",
    undo: "",
  };

  if (props.platform === 'windows') {
    template = { ...template, elevated: false };
  }
  config.value.global_prep_cmd.push(template);
}

function removeCmd(index) {
  config.value.global_prep_cmd.splice(index,1)
}
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-3">
      <label for="locale" class="form-label">{{ $t('config.locale') }}</label>
      <select id="locale" class="form-select" v-model="config.locale">
        <option value="bg">Български (Bulgarian)</option>
        <option value="cs">Čeština (Czech)</option>
        <option value="de">Deutsch (German)</option>
        <option value="en">English</option>
        <option value="en_GB">English, United Kingdom</option>
        <option value="en_US">English, United States</option>
        <option value="es">Español (Spanish)</option>
        <option value="fr">Français (French)</option>
        <option value="hu">Magyar (Hungarian)</option>
        <option value="it">Italiano (Italian)</option>
        <option value="ja">日本語 (Japanese)</option>
        <option value="ko">한국어 (Korean)</option>
        <option value="pl">Polski (Polish)</option>
        <option value="pt">Português (Portuguese)</option>
        <option value="pt_BR">Português, Brasileiro (Portuguese, Brazilian)</option>
        <option value="ru">Русский (Russian)</option>
        <option value="sv">svenska (Swedish)</option>
        <option value="tr">Türkçe (Turkish)</option>
        <option value="uk">Українська (Ukranian)</option>
        <option value="vi">Tiếng Việt (Vietnamese)</option>
        <option value="zh">简体中文 (Chinese Simplified)</option>
        <option value="zh_TW">繁體中文 (Chinese Traditional)</option>
      </select>
      <div class="form-text">{{ $t('config.locale_desc') }}</div>
    </div>

    <!-- Sunshine Name -->
    <div class="mb-3">
      <label for="sunshine_name" class="form-label">{{ $t('config.sunshine_name') }}</label>
      <input type="text" class="form-control" id="sunshine_name" placeholder="Sunshine"
             v-model="config.sunshine_name" />
      <div class="form-text">{{ $t('config.sunshine_name_desc') }}</div>
    </div>

    <!-- Log Level -->
    <div class="mb-3">
      <label for="min_log_level" class="form-label">{{ $t('config.min_log_level') }}</label>
      <select id="min_log_level" class="form-select" v-model="config.min_log_level">
        <option value="0">{{ $t('config.min_log_level_0') }}</option>
        <option value="1">{{ $t('config.min_log_level_1') }}</option>
        <option value="2">{{ $t('config.min_log_level_2') }}</option>
        <option value="3">{{ $t('config.min_log_level_3') }}</option>
        <option value="4">{{ $t('config.min_log_level_4') }}</option>
        <option value="5">{{ $t('config.min_log_level_5') }}</option>
        <option value="6">{{ $t('config.min_log_level_6') }}</option>
      </select>
      <div class="form-text">{{ $t('config.min_log_level_desc') }}</div>
    </div>

    <!-- Global Prep Commands -->
    <div id="global_prep_cmd" class="mb-3 d-flex flex-column">
      <label class="form-label">{{ $t('config.global_prep_cmd') }}</label>
      <div class="form-text">{{ $t('config.global_prep_cmd_desc') }}</div>
      <!-- One row per command from md up; stacked with per-field labels on phones -->
      <div class="mt-3" v-if="config.global_prep_cmd.length > 0">
        <div class="row g-2 d-none d-md-flex fw-semibold border-bottom pb-2">
          <div class="col-md-3 col-xl-2">{{ $t('_common.prep_type') }}</div>
          <div class="col-md"><Play :size="16" /> {{ $t('_common.do_cmd') }}</div>
          <div class="col-md"><Undo :size="16" /> {{ $t('_common.undo_cmd') }}</div>
          <div class="col-auto prep-cmd-elevated" v-if="platform === 'windows'">
            <Shield :size="16" /> {{ $t('_common.run_as') }}
          </div>
          <div class="col-auto prep-cmd-actions"></div>
        </div>
        <div class="row g-2 align-items-end border-bottom py-2" v-for="(c, i) in config.global_prep_cmd">
          <div class="col-12 col-md-3 col-xl-2">
            <label class="form-label small mb-1 d-md-none" :for="'prep-cmd-type-' + i">{{ $t('_common.prep_type') }}</label>
            <select class="form-select" :id="'prep-cmd-type-' + i" v-model="c.type">
              <option value="app">{{ $t('_common.prep_type_app') }}</option>
              <option value="connect">{{ $t('_common.prep_type_connect') }}</option>
            </select>
          </div>
          <div class="col-12 col-md">
            <label class="form-label small mb-1 d-md-none" :for="'prep-cmd-do-' + i"><Play :size="14" /> {{ $t('_common.do_cmd') }}</label>
            <input type="text" class="form-control monospace" :id="'prep-cmd-do-' + i" v-model="c.do" />
          </div>
          <div class="col-12 col-md">
            <label class="form-label small mb-1 d-md-none" :for="'prep-cmd-undo-' + i"><Undo :size="14" /> {{ $t('_common.undo_cmd') }}</label>
            <input type="text" class="form-control monospace" :id="'prep-cmd-undo-' + i" v-model="c.undo" />
          </div>
          <div class="col-auto prep-cmd-elevated align-self-center" v-if="platform === 'windows'">
            <Checkbox :id="'prep-cmd-admin-' + i"
                      label="_common.elevated"
                      desc=""
                      v-model="c.elevated"
            ></Checkbox>
          </div>
          <div class="col-auto prep-cmd-actions ms-auto align-self-center">
            <button class="btn btn-danger" @click="removeCmd(i)">
              <Trash2 :size="16" />
            </button>
            <button class="btn btn-success ms-2" @click="addCmd">
              <Plus :size="16" />
            </button>
          </div>
        </div>
      </div>
      <button class="ms-0 mt-2 btn btn-success" style="margin: 0 auto" @click="addCmd">
        &plus; {{ $t('config.add') }}
      </button>
    </div>

    <!-- Notify Pre-Releases -->
    <Checkbox class="mb-3"
              id="latency_log"
              locale-prefix="config"
              v-model="config.latency_log"
              default="false"
    ></Checkbox>

    <Checkbox class="mb-3"
              id="notify_pre_releases"
              locale-prefix="config"
              v-model="config.notify_pre_releases"
              default="false"
    ></Checkbox>

    <!-- Enable system tray -->
    <Checkbox class="mb-3"
              id="system_tray"
              locale-prefix="config"
              v-model="config.system_tray"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
