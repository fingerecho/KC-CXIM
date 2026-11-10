<template>
  <div class="app-shell" :class="{ 'is-collapsed': appStore.collapsed }">
    <SideBar />
    <div class="app-main">
      <HeaderBar />
      <main class="app-content">
        <router-view v-slot="{ Component }">
          <transition name="fade-slide" mode="out-in">
            <component :is="Component" :key="$route.path" />
          </transition>
        </router-view>
      </main>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import SideBar from './SideBar.vue'
import HeaderBar from './HeaderBar.vue'

const appStore = useAppStore()
</script>

<style scoped>
.app-shell {
  min-height: 100vh;
  background: var(--cx-bg);
}

.app-main {
  min-width: 0;
  margin-left: var(--cx-sidebar-width);
  transition: margin-left 0.26s cubic-bezier(0.4, 0, 0.2, 1);
}

.app-shell.is-collapsed .app-main {
  margin-left: var(--cx-sidebar-collapsed);
}

.app-content {
  padding: 26px 30px 40px;
  max-width: 1680px;
  margin: 0 auto;
}

@media (max-width: 1100px) {
  .app-content {
    padding: 20px 20px 32px;
  }
}

@media (max-width: 820px) {
  .app-main {
    margin-left: var(--cx-sidebar-collapsed);
  }
  .app-content {
    padding: 16px 14px 28px;
  }
}
</style>
