<template>
  <div v-if="state.request" class="confirm-dialog">
    <div class="modal d-block" tabindex="-1" role="dialog" aria-modal="true" aria-labelledby="confirmDialogTitle"
         @click.self="answer(false)" @keydown.esc="answer(false)">
      <div class="modal-dialog modal-dialog-centered">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title" id="confirmDialogTitle">{{ state.request.title }}</h5>
            <button type="button" class="btn-close" :aria-label="$t('_common.close')" @click="answer(false)"></button>
          </div>
          <div class="modal-body">
            <p class="mb-0 confirm-dialog-message">{{ state.request.message }}</p>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" @click="answer(false)">
              {{ state.request.cancelLabel || $t('_common.cancel') }}
            </button>
            <button type="button" class="btn" :class="'btn-' + state.request.variant" ref="confirmButton" @click="answer(true)">
              {{ state.request.confirmLabel }}
            </button>
          </div>
        </div>
      </div>
    </div>
    <div class="modal-backdrop show"></div>
  </div>
</template>

<script>
import { nextTick, reactive } from 'vue'

/**
 * Singleton state shared by every caller of `confirmDialog()` and the mounted dialog, like the notifications.
 * Only one question shows at a time; a new one answers the previous one with false.
 */
export const state = reactive({
  request: null,
})

/**
 * Ask a question in an in-page dialog instead of the browser's `confirm()`.
 *
 * @param {object} options What to ask.
 * @param {string} options.title Dialog title.
 * @param {string} options.message The question.
 * @param {string} options.confirmLabel Label of the button that agrees.
 * @param {string} [options.cancelLabel] Label of the button that declines; "Cancel" when left out.
 * @param {'primary'|'danger'|'warning'} [options.variant] Bootstrap color of the button that agrees.
 * @returns {Promise<boolean>} Resolves to true when the user agrees, false when they decline or close the dialog.
 */
export function confirmDialog({ title, message, confirmLabel, cancelLabel, variant = 'primary' }) {
  if (state.request) {
    state.request.resolve(false)
  }
  return new Promise((resolve) => {
    state.request = { title, message, confirmLabel, cancelLabel, variant, resolve }
  })
}

/**
 * The dialog that shows the question `confirmDialog()` asks. Mount it once per page.
 */
export default {
  setup() {
    return { state }
  },
  data() {
    return {
      returnFocus: null,
    }
  },
  watch: {
    'state.request'(request) {
      if (!request) {
        return
      }
      // Focus the answer, so Enter agrees and Escape declines, and give focus back once answered
      this.returnFocus = document.activeElement
      nextTick(() => this.$refs.confirmButton?.focus())
    },
  },
  methods: {
    /**
     * Close the dialog with an answer.
     *
     * @param {boolean} agreed Whether the user agreed.
     */
    answer(agreed) {
      const request = state.request
      if (!request) {
        return
      }
      state.request = null
      request.resolve(agreed)
      this.returnFocus?.focus?.()
      this.returnFocus = null
    },
  },
}
</script>

<style scoped>
.confirm-dialog .modal {
  z-index: 1060;
}

.confirm-dialog .modal-backdrop {
  z-index: 1055;
}

.confirm-dialog-message {
  white-space: pre-line;
}
</style>
