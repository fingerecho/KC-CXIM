<template>
  <aside class="sidebar" :class="{ 'is-collapsed': appStore.collapsed }">
    <div class="brand">
      <div class="brand-mark">
        <span>铖</span>
      </div>
      <div v-if="!appStore.collapsed" class="brand-text">
        <strong>铖信</strong>
        <span>管理后台</span>
      </div>
    </div>

    <nav class="nav">
      <div v-for="group in menus" :key="group.title" class="nav-group">
        <p v-if="!appStore.collapsed" class="nav-group__title">{{ group.title }}</p>
        <router-link
          v-for="item in group.items"
          :key="item.path"
          :to="item.path"
          class="nav-item"
          :class="{ active: isActive(item.path) }"
          :title="appStore.collapsed ? item.label : undefined"
        >
          <span class="nav-item__icon">
            <el-icon :size="18"><component :is="item.icon" /></el-icon>
          </span>
          <span v-if="!appStore.collapsed" class="nav-item__label">{{ item.label }}</span>
          <span v-if="item.badge && !appStore.collapsed" class="nav-item__badge">{{ item.badge }}</span>
        </router-link>
      </div>
    </nav>

    <div class="sidebar-footer">
      <div v-if="!appStore.collapsed" class="status-card">
        <span class="status-dot" />
        <div>
          <strong>系统运行正常</strong>
          <span>IM 服务 · 数据库在线</span>
        </div>
      </div>
      <button class="collapse-btn" type="button" @click="appStore.toggleCollapse">
        <el-icon :size="18">
          <Expand v-if="appStore.collapsed" />
          <Fold v-else />
        </el-icon>
        <span v-if="!appStore.collapsed">收起菜单</span>
      </button>
    </div>
  </aside>
</template>

<script setup lang="ts">
import { useRoute } from 'vue-router'
import {
  Odometer,
  User,
  UserFilled,
  ChatDotRound,
  ChatLineSquare,
  ChatSquare,
  Warning,
  Promotion,
  FolderOpened,
  Setting,
  Document,
  Avatar,
  Fold,
  Expand,
} from '@element-plus/icons-vue'
import { useAppStore } from '@/stores/app'

const appStore = useAppStore()
const route = useRoute()

const menus = [
  {
    title: '概览',
    items: [{ label: '工作台', path: '/dashboard', icon: Odometer }],
  },
  {
    title: '运营管理',
    items: [
      { label: '用户管理', path: '/users', icon: User },
      { label: '好友关系', path: '/friends', icon: UserFilled },
      { label: '群组管理', path: '/groups', icon: ChatDotRound },
      { label: '消息管理', path: '/messages', icon: ChatLineSquare },
      { label: '内容审核', path: '/moderation', icon: Warning, badge: '3' },
      { label: '推送管理', path: '/push', icon: Promotion },
    ],
  },
  {
    title: '系统',
    items: [
      { label: '文件管理', path: '/files', icon: FolderOpened },
      { label: '系统配置', path: '/config', icon: Setting },
      { label: '审计日志', path: '/audit', icon: Document },
      { label: '管理员与角色', path: '/admins', icon: Avatar },
    ],
  },
]

function isActive(path: string) {
  return route.path === path || route.path.startsWith(`${path}/`)
}
</script>

<style scoped>
.sidebar {
  position: fixed;
  inset: 0 auto 0 0;
  z-index: 40;
  width: var(--cx-sidebar-width);
  display: flex;
  flex-direction: column;
  padding: 0 0 16px;
  background:
    radial-gradient(circle at 20% 0%, rgba(37, 99, 235, 0.28), transparent 46%),
    linear-gradient(180deg, #0b1220 0%, #0f172a 58%, #111c33 100%);
  color: #cbd5e1;
  border-right: 1px solid rgba(148, 163, 184, 0.12);
  overflow: hidden;
  transition: width 0.26s cubic-bezier(0.4, 0, 0.2, 1);
}

.sidebar.is-collapsed {
  width: var(--cx-sidebar-collapsed);
}

.brand {
  display: flex;
  align-items: center;
  gap: 12px;
  height: 78px;
  padding: 0 20px;
  flex-shrink: 0;
}

.brand-mark {
  width: 42px;
  height: 42px;
  border-radius: 13px;
  display: grid;
  place-items: center;
  flex-shrink: 0;
  background: linear-gradient(135deg, #38bdf8 0%, #2563eb 58%, #6d28d9 100%);
  box-shadow: 0 10px 24px rgba(37, 99, 235, 0.45);
  color: #fff;
  font-size: 20px;
  font-weight: 700;
  letter-spacing: -0.04em;
}

.brand-text {
  display: flex;
  flex-direction: column;
  line-height: 1.2;
  white-space: nowrap;
}

.brand-text strong {
  font-size: 18px;
  color: #f8fafc;
  letter-spacing: 0.08em;
}

.brand-text span {
  font-size: 11px;
  color: #64748b;
  letter-spacing: 0.16em;
  margin-top: 3px;
}

.nav {
  flex: 1;
  overflow-y: auto;
  padding: 6px 0 12px;
}

.nav::-webkit-scrollbar {
  width: 4px;
}

.nav::-webkit-scrollbar-thumb {
  background: rgba(148, 163, 184, 0.25);
}

.nav-group {
  margin-bottom: 10px;
}

.nav-group__title {
  margin: 10px 24px 8px;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.16em;
  text-transform: uppercase;
  color: #526079;
}

.nav-item {
  position: relative;
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 3px 12px;
  padding: 11px 13px;
  border-radius: 11px;
  color: #94a3b8;
  font-size: 14px;
  font-weight: 500;
  transition: background 0.2s ease, color 0.2s ease, transform 0.2s ease;
}

.nav-item:hover {
  background: rgba(148, 163, 184, 0.1);
  color: #e2e8f0;
}

.nav-item.active {
  color: #fff;
  background: linear-gradient(95deg, rgba(37, 99, 235, 0.95), rgba(59, 130, 246, 0.55));
  box-shadow: 0 12px 26px rgba(37, 99, 235, 0.32);
}

.nav-item.active::before {
  content: '';
  position: absolute;
  left: -12px;
  top: 50%;
  width: 3px;
  height: 20px;
  transform: translateY(-50%);
  border-radius: 0 4px 4px 0;
  background: #93c5fd;
}

.nav-item__icon {
  display: grid;
  place-items: center;
  flex-shrink: 0;
}

.nav-item__label {
  white-space: nowrap;
}

.nav-item__badge {
  margin-left: auto;
  min-width: 20px;
  height: 20px;
  padding: 0 6px;
  border-radius: 999px;
  background: #f43f5e;
  color: #fff;
  font-size: 11px;
  font-weight: 600;
  display: grid;
  place-items: center;
}

.sidebar.is-collapsed .nav-item {
  justify-content: center;
  padding: 12px 0;
}

.sidebar.is-collapsed .nav-item.active::before {
  display: none;
}

.sidebar.is-collapsed .brand {
  justify-content: center;
  padding: 0;
}

.sidebar-footer {
  padding: 12px 14px 4px;
  flex-shrink: 0;
}

.status-card {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 14px;
  margin-bottom: 10px;
  border-radius: 12px;
  background: rgba(15, 23, 42, 0.55);
  border: 1px solid rgba(148, 163, 184, 0.14);
}

.status-card strong {
  display: block;
  font-size: 12px;
  color: #e2e8f0;
  font-weight: 600;
}

.status-card span {
  font-size: 10px;
  color: #64748b;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #22c55e;
  box-shadow: 0 0 0 4px rgba(34, 197, 94, 0.16);
  animation: pulse 2s infinite;
}

@keyframes pulse {
  0%,
  100% {
    box-shadow: 0 0 0 3px rgba(34, 197, 94, 0.14);
  }
  50% {
    box-shadow: 0 0 0 6px rgba(34, 197, 94, 0.06);
  }
}

.collapse-btn {
  width: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 10px;
  border: none;
  border-radius: 10px;
  background: transparent;
  color: #64748b;
  font-size: 12px;
  cursor: pointer;
  transition: background 0.2s ease, color 0.2s ease;
}

.collapse-btn:hover {
  background: rgba(148, 163, 184, 0.1);
  color: #cbd5e1;
}
</style>
