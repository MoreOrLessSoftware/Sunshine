import { reactive } from 'vue'
import { apiFetch } from './fetch_utils'
import { notifyKey } from './Notification.vue'

/**
 * Watches the connection to Sunshine for every page, and restarts Sunshine in a way the page can follow.
 *
 * `GET /api/health` answers with the ID Sunshine made when it started. While the page is open it is checked every
 * few seconds: no answer means the connection is lost, and a new ID means Sunshine restarted. A restart started with
 * `restartSunshine()` shows as one until Sunshine is back, without reloading the page, so it keeps its state; the
 * other open tabs show it too, then offer a reload.
 */

/** How often to check while connected and the page is visible. */
const CHECK_INTERVAL_MS = 10000
/** How often to check while waiting for Sunshine to come back. */
const RECONNECT_INTERVAL_MS = 1500
/** How long a check waits for an answer. */
const CHECK_TIMEOUT_MS = 4000
/** Checks that must fail in a row before the connection counts as lost, so one dropped request doesn't. */
const FAILURES_BEFORE_LOST = 2
/** How long a restart may leave the same process answering before it counts as not having happened. */
const RESTART_START_TIMEOUT_MS = 20000
/** Name of the channel that tells the other tabs of this web UI about a restart. */
const CHANNEL_NAME = 'sunshine-server-status'

/**
 * The connection state every page shares.
 *
 * - `online`: Sunshine answers.
 * - `lost`: Sunshine stopped answering, for a reason this page doesn't know.
 * - `restarting`: a restart started here or in another tab, waiting for Sunshine to come back.
 * - `restarted`: Sunshine came back as a new process that this page didn't restart, so what it shows may be stale.
 */
export const status = reactive({
  state: 'online',
  since: Date.now(),  // When the current state began
})

let started = false
let timer = null
let checking = null
let failures = 0
let bootId = null
let restart = null  // {fromBootId, wentDown, requesting, ours, settle} while restarting
let channel = null

/**
 * Change the state, remembering when.
 *
 * @param {string} state The new state.
 */
function setState(state) {
  if (status.state !== state) {
    status.state = state
    status.since = Date.now()
  }
}

/**
 * Ask Sunshine which process is running.
 *
 * @returns {Promise<string|null>} The boot ID, or null when Sunshine didn't answer.
 */
async function fetchBootId() {
  const controller = new AbortController()
  const timeout = setTimeout(() => controller.abort(), CHECK_TIMEOUT_MS)
  try {
    const response = await fetch('./api/health', {cache: 'no-store', signal: controller.signal})
    if (!response.ok) {
      return null
    }
    const body = await response.json()
    return typeof body.boot_id === 'string' ? body.boot_id : null
  } catch {
    return null
  } finally {
    clearTimeout(timeout)
  }
}

/**
 * Handle an answer from Sunshine.
 *
 * @param {string} id The boot ID it answered with.
 */
function answered(id) {
  failures = 0
  if (bootId === null) {
    bootId = id
  }

  if (status.state === 'restarting') {
    // Until Sunshine has taken the restart request, an answer from it says nothing about the restart
    if (!restart || restart.requesting) {
      return
    }
    if (id !== restart.fromBootId) {
      // Back as the new process. A restart from this page leaves it as it is, since the page made the change;
      // one from another tab may have changed what this page shows, so it offers a reload.
      bootId = id
      const finished = restart
      restart = null
      if (finished.ours) {
        setState('online')
        notifyKey.success('connection.restart_done')
      } else {
        setState('restarted')
      }
      finished.settle?.(true)
      return
    }
    // The same process answers: after a gap the connection only dropped, and without one it never went down
    if (restart.wentDown || Date.now() - status.since > RESTART_START_TIMEOUT_MS) {
      const failed = restart
      restart = null
      setState('online')
      notifyKey.error('connection.restart_failed')
      failed.settle?.(false)
    }
    return
  }

  if (id !== bootId) {
    bootId = id
    setState('restarted')
  } else if (status.state === 'lost') {
    setState('online')
  }
}

/**
 * Handle a check Sunshine didn't answer.
 */
function unanswered() {
  failures++
  if (status.state === 'restarting') {
    if (restart) {
      restart.wentDown = true
    }
  } else if (status.state !== 'lost' && failures >= FAILURES_BEFORE_LOST) {
    setState('lost')
  }
}

/**
 * Check the connection now, then schedule the next check.
 *
 * @returns {Promise<void>} Resolves once the check is done.
 */
function check() {
  if (!checking) {
    checking = (async () => {
      clearTimeout(timer)
      const id = await fetchBootId()
      if (id === null) {
        unanswered()
      } else {
        answered(id)
      }
      checking = null
      schedule()
    })()
  }
  return checking
}

/**
 * Schedule the next check: soon while waiting for Sunshine, otherwise only while the page is visible.
 */
function schedule() {
  clearTimeout(timer)
  if (status.state === 'lost' || status.state === 'restarting' || failures > 0) {
    timer = setTimeout(check, RECONNECT_INTERVAL_MS)
  } else if (document.visibilityState === 'visible') {
    timer = setTimeout(check, CHECK_INTERVAL_MS)
  }
}

/**
 * Show a restart, and wait for Sunshine to come back as a new process.
 *
 * @param {string|null} fromBootId Boot ID of the process that is restarting, or null when it isn't known yet.
 * @param {object} [options] How the restart started.
 * @param {boolean} [options.requesting] Whether the restart request is still on its way, so answers don't count yet.
 * @param {boolean} [options.ours] Whether this page asked for the restart, rather than another tab.
 * @param {function(boolean): void} [options.settle] Called with whether Sunshine came back as a new process.
 */
function beginRestart(fromBootId, {requesting = false, ours = false, settle = null} = {}) {
  restart = {fromBootId, wentDown: false, requesting, ours, settle}
  failures = 0
  setState('restarting')
  schedule()
}

/**
 * Start watching the connection. Pages call it once, through the component that shows the state.
 */
export function startMonitor() {
  if (started) {
    return
  }
  started = true

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') {
      check()
    }
  })

  if (typeof BroadcastChannel !== 'undefined') {
    channel = new BroadcastChannel(CHANNEL_NAME)
    channel.addEventListener('message', (event) => {
      // Another tab restarted the Sunshine this page talks to
      if (event.data?.type === 'restarting' && event.data.bootId === bootId && status.state !== 'restarting') {
        beginRestart(bootId)
      }
    })
  }

  check()
}

/**
 * Check the connection right away, after a request failed without an answer.
 */
export function reportNetworkError() {
  if (started && status.state === 'online') {
    check()
  }
}

/**
 * Check the connection right away, such as from a Retry button.
 *
 * @returns {Promise<void>} Resolves once the check is done.
 */
export function checkNow() {
  return check()
}

/**
 * Forget that Sunshine restarted without this page, once the user has seen it.
 */
export function acknowledgeRestart() {
  if (status.state === 'restarted') {
    setState('online')
  }
}

/**
 * Stop showing a restart that is taking too long. The page keeps checking, and shows the connection as lost until
 * Sunshine answers.
 */
export function stopWaitingForRestart() {
  if (status.state === 'restarting') {
    const abandoned = restart
    restart = null
    setState('lost')
    schedule()
    abandoned?.settle?.(false)
  }
}

/**
 * Restart Sunshine, and show that it is restarting in this and the other open tabs until it is back.
 *
 * The page isn't reloaded, so it keeps its state; refresh what the restart changes once this resolves.
 *
 * @returns {Promise<boolean>} Resolves with true once Sunshine is back as a new process, or false when it didn't
 *   restart or the user stopped waiting, which the page already shows. Rejects with the reason when Sunshine
 *   refused the restart.
 */
export async function restartSunshine() {
  startMonitor()
  // Show the restart right away: Sunshine exits without answering, so the request can take seconds to end
  let settle
  const finished = new Promise((resolve) => {
    settle = resolve
  })
  beginRestart(bootId, {requesting: true, ours: true, settle})
  if (bootId === null) {
    await check()
    if (restart) {
      restart.fromBootId = bootId
    }
  }

  let response = null
  try {
    response = await apiFetch('./api/restart', {method: 'POST', headers: {'Content-Type': 'application/json'}})
  } catch {
    // Sunshine can exit before it answers, which is what a restart looks like
  }
  if (response && !response.ok) {
    let error = `HTTP ${response.status}`
    try {
      error = (await response.json()).error || error
    } catch {
      // No JSON body
    }
    // Sunshine refused, so it isn't restarting after all
    if (status.state === 'restarting') {
      restart = null
      setState('online')
    }
    schedule()
    throw new Error(error)
  }

  // Stop Waiting may have ended the restart while the request was on its way
  if (restart) {
    restart.requesting = false
    channel?.postMessage({type: 'restarting', bootId: restart.fromBootId})
  }
  return finished
}

// Plugin panels are framed from the same origin and can offer a restart through this, even before the page mounts
window.sunshine = {...window.sunshine, restart: restartSunshine}
