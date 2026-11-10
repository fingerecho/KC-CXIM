<template>
  <header class="header">
    <div class="header-left">
      <div class="crumb">
        <span class="crumb-home">铖信</span>
        <el-icon :size="12" class="crumb-sep"><ArrowRight /></el-icon>
        <span class="crumb-current">{{ currentTitle }}</span>
      </div>
      <h2 class="header-title">{{ currentTitle }}</h2>
    </div>

    <div class="header-right">
      <el-input
        v-model="keyword"
        class="header-search"
        placeholder="搜索用户、消息、群组…"
        clearable
        @keyup.enter="handleSearch"
      >
        <template #prefix>
          <el-icon><Search /></el-icon>
        </template>
        <template #suffix>
          <span class="search-kbd">⌘K</span>
        </template>
      </el-input>

      <button class="icon-btn" type="button" title="全屏" @click="toggleFullscreen">
        <el-icon :size="18"><FullScreen /></el-icon>
      </button>

      <el-tooltip content="3 条待处理审核" placement="bottom">
        <button class="icon-btn" type="button" title="通知">
          <el-badge is-dot class="header-badge">
            <el-icon :size="18"><Bell /></el-icon>
          </el-badge>
        </button>
      </el-tooltip>

      <div class="header-divider" />

      <el-dropdown trigger="click" @command="handleCommand">
        <div class="user-chip">
          <div class="user-avatar">{{ auth.initial }}</div>
          <div class="user-meta">
            <strong>{{ auth.displayName }}</strong>
            <span>{{ auth.user?.roleName || '管理员' }}</span>
          </div>
          <el-icon :size="14" class="user-arrow"><ArrowDown /></el-icon>
        </div>
        <template #dropdown>
          <el-dropdown-menu>
            <el-dropdown-item command="profile">
              <el-icon><User /></el-icon>个人资料
            </el-dropdown-item>
            <el-dropdown-item command="settings">
              <el-icon><Setting /></el-icon>系统设置
            </el-dropdown-item>
            <el-dropdown-item divided command="logout">
              <el-icon><SwitchButton /></el-icon>退出登录
            </el-dropdown-item>
          </el-dropdown-menu>
        </template>
      </el-dropdown>
    </div>
  </header>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import {
  ArrowRight,
  ArrowDown,
  Search,
  FullScreen,
  Bell,
  User,
  Setting,
  SwitchButton,
} from '@element-plus/icons-vue'
import { useAuthStore } from '@/stores/auth'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()
const keyword = ref('')

const currentTitle = computed(() => (route.meta.title as string) || '工作台')

function handleSearch() {
  if (!keyword.value.trim()) return
  ElMessage.info(`演示环境：暂未接入全局搜索（关键词：${keyword.value.trim()}）`)
}

async function toggleFullscreen() {
  try {
    if (!document.fullscreenElement) {
      await document.documentElement.requestFullscreen()
    } else {
      await document.exitFullscreen()
    }
  } catch {
    ElMessage.warning('当前浏览器不支持全屏')
  }
}

function handleCommand(command: string) {
  if (command === 'logout') {
    auth.logout()
    ElMessage.success('已安全退出')
    router.push('/login')
    return
  }
  if (command === 'profile') {
    ElMessage.info('个人资料模块即将开放')
    return
  }
  ElMessage.info('系统设置模块即将开放')
}
</script>

<style scoped>
.header {
  position: sticky;
  top: 0;
  z-index: 30;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 20px;
  height: 74px;
  padding: 0 30px;
  background: rgba(255, 255, 255, 0.86);
  backdrop-filter: blur(16px);
  border-bottom: 1px solid var(--cx-border);
}

.header-left {
  min-width: 0;
}

.crumb {
  display: flex;
  align-items: center;
  gap: 7px;
  font-size: 11px;
  color: var(--cx-text-muted);
  margin-bottom: 3px;
}

.crumb-home {
  color: #64748b;
  letter-spacing: 0.08em;
}

.crumb-sep {
  color: #cbd5e1;
}

.crumb-current {
  color: #2563eb;
  font-weight: 500;
}

.header-title {
  margin: 0;
  font-size: 18px;
  font-weight: 650;
  letter-spacing: -0.01em;
  color: var(--cx-ink);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 10px;
}

.header-search {
  width: 268px;
}

.header-search :deep(.el-input__wrapper) {
  border-radius: 11px;
  background: #f4f7fb;
  box-shadow: inset 0 0 0 1px transparent;
  transition: box-shadow 0.2s ease, background 0.2s ease;
}

.header-search :deep(.el-input__wrapper.is-focus) {
  background: #fff;
  box-shadow: inset 0 0 0 1px #bfdbfe, 0 0 0 4px rgba(37, 99, 235, 0.08);
}

.search-kbd {
  font-size: 10px;
  color: #94a3b8;
  border: 1px solid #e2e8f0;
  border-radius: 5px;
  padding: 1px 5px;
  background: #fff;
}

.icon-btn {
  width: 40px;
  height: 40px;
  display: grid;
  place-items: center;
  border: 1px solid transparent;
  border-radius: 11px;
  background: transparent;
  color: #64748b;
  cursor: pointer;
  transition: background 0.2s ease, color 0.2s ease, border-color 0.2s ease;
}

.icon-btn:hover {
  background: #f4f7fb;
  color: #2563eb;
  border-color: #e2e8f0;
}

.header-badge :deep(.el-badge__content.is-dot) {
  width: 7px;
  height: 7px;
  right: 1px;
  top: 1px;
}

.header-divider {
  width: 1px;
  height: 26px;
  background: var(--cx-border);
  margin: 0 4px;
}

.user-chip {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 5px 8px 5px 5px;
  border-radius: 12px;
  cursor: pointer;
  transition: background 0.2s ease;
  outline: none;
}

.user-chip:hover {
  background: #f4f7fb;
}

.user-avatar {
  width: 36px;
  height: 36px;
  border-radius: 11px;
  display: grid;
  place-items: center;
  background: linear-gradient(135deg, #38bdf8, #2563eb 60%, #7c3aed);
  color: #fff;
  font-size: 14px;
  font-weight: 700;
  box-shadow: 0 6px 16px rgba(37, 99, 235, 0.28);
}

.user-meta {
  display: flex;
  flex-direction: column;
  line-height: 1.2;
}

.user-meta strong {
  font-size: 13px;
  color: var(--cx-ink);
  font-weight: 600;
}

.user-meta span {
  font-size: 11px;
  color: var(--cx-text-muted);
  margin-top: 2px;
}

.user-arrow {
  color: #94a3b8;
}

@media (max-width: 1180px) {
  .header-search {
    width: 180px;
  }
  .user-meta {
    display: none;
  }
}

@media (max-width: 820px) {
  .header {
    padding: 0 16px;
  }
  .header-search {
    display: none;
  }
}
</style>
