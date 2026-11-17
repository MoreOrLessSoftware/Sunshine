<template>
  <div>
    <!-- A restart this or another tab started, until Sunshine is back -->
    <div v-if="status.state === 'restarting'" class="connection-status-restart">
      <div class="modal d-block" tabindex="-1" role="dialog" aria-modal="true" aria-labelledby="connectionRestartTitle">
        <div class="modal-dialog modal-dialog-centered">
          <div class="modal-content">
            <div class="modal-body d-flex gap-3 align-items-start">
              <div class="spinner-border text-primary flex-shrink-0" aria-hidden="true"></div>
              <div>
                <h5 class="mb-1" id="connectionRestartTitle">{{ $t('connection.restarting_title') }}</h5>
                <p class="mb-0" role="status">
                  {{ $t('connection.restarting_desc') }}
                  <span class="text-body-secondary">{{ $t('connection.elapsed', {seconds: elapsed}) }}</span>
                </p>
                <p v-if="elapsed >= SLOW_RESTART_SECONDS" class="mt-2 mb-0 text-body-secondary">{{ $t('connection.restarting_slow') }}</p>
              </div>
            </div>
            <div v-if="elapsed >= SLOW_RESTART_SECONDS" class="modal-footer">
              <button type="button" class="btn btn-secondary" @click="stopWaitingForRestart">{{ $t('connection.stop_waiting') }}</button>
            </div>
          </div>
        </div>
      </div>
      <div class="modal-backdrop show"></div>
    </div>

    <!-- Sunshine stopped answering for a reason this page doesn't know -->
    <div v-else-if="status.state === 'lost'" class="alert alert-warning connection-status-banner d-flex align-items-center gap-2 mb-0 rounded-0" role="alert">
      <div class="spinner-border spinner-border-sm flex-shrink-0" aria-hidden="true"></div>
      <span class="flex-grow-1">{{ $t(elapsed >= LONG_LOSS_SECONDS ? 'connection.lost_long' : 'connection.lost') }}</span>
      <button v-if="elapsed >= LONG_LOSS_SECONDS" type="button" class="btn btn-sm btn-outline-dark" @click="checkNow">{{ $t('connection.retry') }}</button>
    </div>

    <!-- Back as a new process this page didn't restart: offer a reload instead of losing unsaved edits -->
    <div v-else-if="status.state === 'restarted'" class="alert alert-info connection-status-banner d-flex align-items-center gap-2 mb-0 rounded-0" role="status">
      <span class="flex-grow-1">{{ $t('connection.restarted') }}</span>
      <button type="button" class="btn btn-sm btn-primary" @click="reload">{{ $t('connection.reload') }}</button>
      <button type="button" class="btn-close" :aria-label="$t('_common.dismiss')" @click="acknowledgeRestart"></button>
    </div>
  </div>
</template>

<script>
import { acknowledgeRestart, checkNow, startMonitor, status, stopWaitingForRestart } from './server_status'

/**
 * Shows when the connection to Sunshine is lost, when it restarts, and when it came back as a new process.
 * Mount it once per page; it starts the shared connection monitor.
 */
export default {
  data() {
    return {
      SLOW_RESTART_SECONDS: 60,
      LONG_LOSS_SECONDS: 60,
      now: Date.now(),
      ticker: null,
    }
  },
  setup() {
    return { status }
  },
  computed: {
    /** Whole seconds in the current state. */
    elapsed() {
      return Math.max(0, Math.floor((this.now - status.since) / 1000))
    },
  },
  watch: {
    'status.state': {
      immediate: true,
      handler(state) {
        // Count the seconds only while there is something to show
        clearInterval(this.ticker)
        this.ticker = null
        this.now = Date.now()
        if (state === 'lost' || state === 'restarting') {
          this.ticker = setInterval(() => {
            this.now = Date.now()
          }, 1000)
        }
      },
    },
  },
  mounted() {
    startMonitor()
  },
  unmounted() {
    clearInterval(this.ticker)
  },
  methods: {
    acknowledgeRestart,
    checkNow,
    stopWaitingForRestart,
    reload() {
      window.location.reload()
    },
  },
}
</script>

<style scoped>
.connection-status-restart .modal {
  z-index: 1070;
}

.connection-status-restart .modal-backdrop {
  z-index: 1065;
}

/* Stays in view wherever the page is scrolled */
.connection-status-banner {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  z-index: 1060;
}
</style>
