<template>
  <TransitionGroup name="notification" tag="div" class="notification-container">
    <div
      v-for="n in state.notifications"
      :key="n.id"
      class="alert d-flex align-items-start gap-2 mb-2"
      :class="'alert-' + n.type"
      role="alert"
      @mouseenter="pauseDismiss(n.id)"
      @mouseleave="resumeDismiss(n.id)"
      @focusin="pauseDismiss(n.id)"
      @focusout="resumeDismiss(n.id)"
    >
      <component :is="iconFor(n.type)" :size="18" class="icon flex-shrink-0 mt-1"></component>
      <div class="flex-grow-1">
        <div v-if="n.titleKey || n.title">
          <strong>{{ n.titleKey ? $t(n.titleKey) : n.title }}</strong>
        </div>
        <span>{{ n.messageKey ? $t(n.messageKey) : n.message }}</span>
      </div>
      <button type="button" class="btn-close" :aria-label="$t('_common.dismiss')" @click="dismiss(n.id)"></button>
    </div>
  </TransitionGroup>
</template>

<script>
import { reactive } from 'vue'
import { AlertCircle, AlertTriangle, CheckCircle, Info } from '@lucide/vue'

/**
 * Singleton reactive notification state shared across the app instance.
 * Using reactive() at module scope means all consumers — including plain JS
 * modules like fetch_utils.js — mutate the same reactive object, and any
 * mounted Notification component will update automatically.
 */
export const state = reactive({
  notifications: [],
  _nextId: 1,
})

/**
 * How long each kind of notification stays before it clears itself. Errors stay longest, since they need reading.
 */
const DISMISS_AFTER_MS = { success: 5000, info: 6000, warning: 8000, danger: 10000 }

/**
 * Timers that clear the notifications, by ID: the pending timeout, and the time left when it was paused.
 */
const timers = new Map()

/**
 * Remove a notification and its timer.
 *
 * @param {number} id - The notification's ID.
 */
function dismiss(id) {
  clearTimeout(timers.get(id)?.timeout)
  timers.delete(id)
  const idx = state.notifications.findIndex(n => n.id === id)
  if (idx !== -1) state.notifications.splice(idx, 1)
}

/**
 * Clear a notification after a delay.
 *
 * @param {number} id - The notification's ID.
 * @param {number} ms - How long to wait.
 */
function startDismiss(id, ms) {
  timers.set(id, { timeout: setTimeout(() => dismiss(id), ms), due: Date.now() + ms, remaining: ms })
}

/**
 * Keep a notification while the pointer or keyboard focus is on it.
 *
 * @param {number} id - The notification's ID.
 */
function pauseDismiss(id) {
  const timer = timers.get(id)
  if (timer && timer.timeout !== null) {
    clearTimeout(timer.timeout)
    timer.timeout = null
    timer.remaining = Math.max(timer.due - Date.now(), 0)
  }
}

/**
 * Carry on clearing a notification once the pointer and focus have left it, with at least a moment to spare.
 *
 * @param {number} id - The notification's ID.
 */
function resumeDismiss(id) {
  const timer = timers.get(id)
  if (timer && timer.timeout === null) {
    startDismiss(id, Math.max(timer.remaining, 2000))
  }
}

/**
 * Show a notification, and clear it after the delay for its kind.
 *
 * @param {object} notification - The notification, with its `type`.
 */
function add(notification) {
  state.notifications.push(notification)
  startDismiss(notification.id, DISMISS_AFTER_MS[notification.type] ?? DISMISS_AFTER_MS.info)
}

/**
 * Push a notification using raw strings.
 *
 * @param {'danger'|'warning'|'success'|'info'} type - Bootstrap color variant.
 * @param {string} message - The notification body text.
 * @param {string} [title] - Optional bold title prefix.
 */
function push(type, message, title) {
  add({ id: state._nextId++, type, message, title: title || null, messageKey: null, titleKey: null })
}

/**
 * Push a notification using i18n translation keys.
 *
 * @param {'danger'|'warning'|'success'|'info'} type - Bootstrap color variant.
 * @param {string} messageKey - i18n key for the notification body.
 * @param {string} [titleKey] - Optional i18n key for the bold title prefix.
 */
function pushKey(type, messageKey, titleKey) {
  add({ id: state._nextId++, type, message: null, title: null, messageKey, titleKey: titleKey || null })
}

/**
 * Map a Bootstrap color variant to its corresponding @lucide/vue icon name.
 *
 * @param {'danger'|'warning'|'success'|'info'} type - Bootstrap color variant.
 * @returns {string} The lucide icon component name.
 */
function iconFor(type) {
  return { danger: 'AlertCircle', warning: 'AlertTriangle', success: 'CheckCircle', info: 'Info' }[type] || 'Info'
}

/**
 * Convenience helpers for the four common variants using raw strings.
 */
export const notify = {
  /**
   * @param {string} message - The notification body text.
   * @param {string} [title] - Optional bold title shown above the message.
   */
  error: (message, title) => push('danger', message, title),
  /**
   * @param {string} message - The notification body text.
   * @param {string} [title] - Optional bold title shown above the message.
   */
  warning: (message, title) => push('warning', message, title),
  /**
   * @param {string} message - The notification body text.
   * @param {string} [title] - Optional bold title shown above the message.
   */
  success: (message, title) => push('success', message, title),
  /**
   * @param {string} message - The notification body text.
   * @param {string} [title] - Optional bold title shown above the message.
   */
  info: (message, title) => push('info', message, title),
}

/**
 * Convenience helpers for the four common variants using i18n keys.
 */
export const notifyKey = {
  /**
   * @param {string} messageKey - i18n key for the notification body text.
   * @param {string} [titleKey] - Optional i18n key for the bold title shown above the message.
   */
  error: (messageKey, titleKey) => pushKey('danger', messageKey, titleKey),
  /**
   * @param {string} messageKey - i18n key for the notification body text.
   * @param {string} [titleKey] - Optional i18n key for the bold title shown above the message.
   */
  warning: (messageKey, titleKey) => pushKey('warning', messageKey, titleKey),
  /**
   * @param {string} messageKey - i18n key for the notification body text.
   * @param {string} [titleKey] - Optional i18n key for the bold title shown above the message.
   */
  success: (messageKey, titleKey) => pushKey('success', messageKey, titleKey),
  /**
   * @param {string} messageKey - i18n key for the notification body text.
   * @param {string} [titleKey] - Optional i18n key for the bold title shown above the message.
   */
  info: (messageKey, titleKey) => pushKey('info', messageKey, titleKey),
}

export default {
  components: { AlertCircle, AlertTriangle, CheckCircle, Info },

  setup() {
    return { state, dismiss, pauseDismiss, resumeDismiss, iconFor }
  },
}
</script>

<style scoped>
.notification-container {
  position: fixed;
  bottom: 1rem;
  right: 1rem;
  left: 1rem;
  z-index: 1090;
}

@media (min-width: 576px) {
  .notification-container {
    left: auto;
    width: 22rem;
  }
}

/* An empty container doesn't block clicks on the page under it */
.notification-container:empty {
  display: none;
}

.notification-enter-active,
.notification-leave-active {
  transition: opacity 0.3s ease, transform 0.3s ease;
}

.notification-enter-from,
.notification-leave-to {
  opacity: 0;
  transform: translateX(1rem);
}

@media (prefers-reduced-motion: reduce) {
  .notification-enter-active,
  .notification-leave-active {
    transition: none;
  }
}
</style>
