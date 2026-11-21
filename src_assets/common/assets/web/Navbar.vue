<template>
  <div>
    <nav class="navbar navbar-expand navbar-sunshine">
      <div class="container-fluid">
        <a class="navbar-brand" href="./" title="Sunshine">
          <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine">
        </a>
        <!-- Links that fit on one line; the rest move to the overflow menu -->
        <div class="navbar-priority" ref="priority">
          <ul class="navbar-nav">
            <li class="nav-item" v-for="item in visibleMainItems" :key="item.href">
              <a class="nav-link" :class="{active: isActive(item.href)}" :href="item.href">
                <component :is="item.icon" :size="18" class="icon"></component>
                {{ $t(item.label) }}
              </a>
            </li>
          </ul>
          <ul class="navbar-nav ms-auto" v-if="featuredVisible">
            <li class="nav-item">
              <a class="nav-link" :class="{active: isActive(featuredItem.href)}" :href="featuredItem.href">
                <component :is="featuredItem.icon" :size="18" class="icon"></component>
                {{ $t(featuredItem.label) }}
              </a>
            </li>
          </ul>
        </div>
        <ul class="navbar-nav">
          <li class="nav-item dropdown" v-if="overflowItems.length > 0">
            <button class="nav-link dropdown-toggle" type="button" id="navbarMoreMenu"
                    :class="{active: overflowItems.some((item) => isActive(item.href))}"
                    data-bs-toggle="dropdown" aria-expanded="false"
                    :aria-label="$t('navbar.more')" :title="$t('navbar.more')">
              <Menu :size="18" class="icon"></Menu>
            </button>
            <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="navbarMoreMenu">
              <li v-for="item in overflowItems" :key="item.href">
                <a class="dropdown-item d-flex align-items-center" :class="{active: isActive(item.href)}" :href="item.href">
                  <component :is="item.icon" :size="18" class="icon"></component>
                  {{ $t(item.label) }}
                </a>
              </li>
            </ul>
          </li>
          <li class="nav-item">
            <ThemeToggle/>
          </li>
          <li class="nav-item dropdown">
            <button class="nav-link dropdown-toggle" type="button" id="navbarUserMenu"
                    data-bs-toggle="dropdown" aria-expanded="false" aria-label="User menu" title="User menu">
              <CircleUserRound :size="18" class="icon"></CircleUserRound>
            </button>
            <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="navbarUserMenu">
              <li>
                <a class="dropdown-item d-flex align-items-center" :class="{active: isActive('./password')}" href="./password">
                  <Shield :size="18" class="icon"></Shield>
                  {{ $t('navbar.password') }}
                </a>
              </li>
              <li><hr class="dropdown-divider"></li>
              <li>
                <button type="button" class="dropdown-item d-flex align-items-center" @click="logout">
                  <LogOut :size="18" class="icon"></LogOut>
                  {{ $t('navbar.logout') }}
                </button>
              </li>
            </ul>
          </li>
        </ul>
        <!-- Invisible copy of every link and the overflow button, used to measure their natural widths -->
        <div class="navbar-measure-clip" aria-hidden="true">
          <ul class="navbar-nav navbar-measure" ref="measure">
            <li class="nav-item" v-for="item in navItems" :key="item.href">
              <span class="nav-link">
                <component :is="item.icon" :size="18" class="icon"></component>
                {{ $t(item.label) }}
              </span>
            </li>
            <li class="nav-item">
              <span class="nav-link dropdown-toggle">
                <Menu :size="18" class="icon"></Menu>
              </span>
            </li>
          </ul>
        </div>
      </div>
    </nav>
    <Notification></Notification>
    <ConfirmDialog></ConfirmDialog>
    <ConnectionStatus></ConnectionStatus>
  </div>
</template>

<script>
import { markRaw } from 'vue'
import { CircleUserRound, Home, Info, Layers, Lock, LogOut, Menu, Puzzle, Settings, Shield, SquareMenu, Star } from '@lucide/vue'
import ThemeToggle from './ThemeToggle.vue'
import Notification from './Notification.vue'
import ConfirmDialog from './ConfirmDialog.vue'
import ConnectionStatus from './ConnectionStatus.vue'

/**
 * Navigation links in priority order. When the bar is too narrow, links leave from the end of this list first.
 * The last entry (Featured Apps) is shown on the right of the bar while there is room for it.
 */
const NAV_ITEMS = [
  {href: './', icon: Home, label: 'navbar.home'},
  {href: './pin', icon: Lock, label: 'navbar.pin'},
  {href: './apps', icon: Layers, label: 'navbar.applications'},
  {href: './client-commands', icon: SquareMenu, label: 'navbar.client_commands'},
  {href: './plugins', icon: Puzzle, label: 'navbar.plugins'},
  {href: './config', icon: Settings, label: 'navbar.configuration'},
  {href: './troubleshooting', icon: Info, label: 'navbar.troubleshoot'},
  {href: './featured', icon: Star, label: 'navbar.featured'},
].map((item) => ({...item, icon: markRaw(item.icon)}))

/**
 * Normalize a link or location path so that `/apps/` and `/apps` compare equal.
 *
 * @param {string} path The path to normalize.
 * @returns {string} The path without a trailing slash, or `/` for the root.
 */
function normalizePath(path) {
  return path.replace(/\/$/, '') || '/'
}

export default {
  components: {
    ThemeToggle,
    Notification,
    ConfirmDialog,
    ConnectionStatus,
    CircleUserRound,
    LogOut,
    Menu,
    Shield
  },
  data() {
    return {
      navItems: NAV_ITEMS,
      visibleCount: NAV_ITEMS.length,
      currentPath: normalizePath(globalThis.location.pathname),
    }
  },
  computed: {
    /**
     * @returns {Object} The Featured Apps link, the last entry in the priority list.
     */
    featuredItem() {
      return this.navItems.at(-1)
    },
    /**
     * @returns {Object[]} The left-hand links that fit on the bar.
     */
    visibleMainItems() {
      return this.navItems.slice(0, Math.min(this.visibleCount, this.navItems.length - 1))
    },
    /**
     * @returns {boolean} Whether the Featured Apps link fits on the bar.
     */
    featuredVisible() {
      return this.visibleCount >= this.navItems.length
    },
    /**
     * @returns {Object[]} The links that did not fit and are listed in the overflow menu.
     */
    overflowItems() {
      return this.navItems.slice(this.visibleCount)
    },
  },
  created() {
    console.log("Header mounted!")
  },
  mounted() {
    this.resizeObserver = new ResizeObserver(() => this.fitItems())
    this.resizeObserver.observe(this.$refs.priority)
    this.resizeObserver.observe(this.$refs.measure)
    document.fonts?.ready.then(() => this.fitItems())
  },
  beforeUnmount() {
    this.resizeObserver?.disconnect()
  },
  methods: {
    /**
     * Check whether a link points at the current page.
     *
     * @param {string} href The link's relative address.
     * @returns {boolean} True when the link is for the page being shown.
     */
    isActive(href) {
      return normalizePath(new URL(href, globalThis.location.href).pathname) === this.currentPath
    },
    /**
     * Show as many links as fit on one line, in priority order, and move the rest to the overflow menu.
     * Widths come from the invisible measuring copy, so they don't depend on which links are shown.
     */
    fitItems() {
      const priority = this.$refs.priority
      const measure = this.$refs.measure
      if (!priority || !measure) {
        return
      }

      const widths = Array.from(measure.children, (child) => child.getBoundingClientRect().width)
      const moreWidth = widths.pop()
      // The overflow button takes its space from the priority area, so add it back when it is showing.
      const available = priority.getBoundingClientRect().width + (this.overflowItems.length > 0 ? moreWidth : 0)
      const total = widths.reduce((sum, width) => sum + width, 0)
      if (total <= available + 0.5) {
        this.visibleCount = widths.length
        return
      }

      const budget = available - moreWidth
      let used = 0
      let count = 0
      while (count < widths.length && used + widths[count] <= budget + 0.5) {
        used += widths[count]
        count++
      }
      this.visibleCount = count
    },
    /**
     * Log out by replacing the browser's cached credentials, then go to the logout page.
     */
    logout() {
      const cacheBuster = Date.now().toString()
      const logoutPageUrl = new URL('/logout', globalThis.location.href)
      const request = new XMLHttpRequest()
      const finish = () => {
        globalThis.location.replace(logoutPageUrl.toString())
      }

      request.open('GET', '/', true, 'sunshine-logout', cacheBuster)
      request.setRequestHeader('Cache-Control', 'no-store')
      request.onload = finish
      request.onerror = finish
      request.ontimeout = finish
      request.timeout = 5000
      request.send()
    }
  }
}
</script>
