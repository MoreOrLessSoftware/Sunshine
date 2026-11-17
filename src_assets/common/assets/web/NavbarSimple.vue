<template>
  <div>
    <nav class="navbar navbar-sunshine">
      <div class="container-fluid">
        <span class="navbar-brand" title="Sunshine">
          <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine">
        </span>
        <ThemeToggle/>
      </div>
    </nav>
    <ConnectionStatus></ConnectionStatus>
  </div>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'
import ConnectionStatus from './ConnectionStatus.vue'

export default {
  components: {
    ThemeToggle,
    ConnectionStatus,
  },
}
</script>
