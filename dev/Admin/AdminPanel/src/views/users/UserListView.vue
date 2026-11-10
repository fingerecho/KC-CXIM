<template>
  <div class="users-page">
    <div class="page-header">
      <div>
        <h1 class="page-title">用户管理</h1>
        <p class="page-subtitle">查询用户资料、管理账号状态与安全信息</p>
      </div>
      <div class="header-actions">
        <el-button plain @click="handleExport">
          <el-icon><Download /></el-icon>
          导出数据
        </el-button>
        <el-button type="primary" @click="handleAdd">
          <el-icon><Plus /></el-icon>
          新增用户
        </el-button>
      </div>
    </div>

    <div class="mini-stats">
      <div class="mini-stat">
        <span class="mini-icon blue"><el-icon><User /></el-icon></span>
        <div>
          <strong>{{ pagination.total }}</strong>
          <span>用户总数</span>
        </div>
      </div>
      <div class="mini-stat">
        <span class="mini-icon emerald"><el-icon><CircleCheck /></el-icon></span>
        <div>
          <strong>{{ activeCount }}</strong>
          <span>本页正常</span>
        </div>
      </div>
      <div class="mini-stat">
        <span class="mini-icon rose"><el-icon><CircleClose /></el-icon></span>
        <div>
          <strong>{{ bannedCount }}</strong>
          <span>本页封禁</span>
        </div>
      </div>
      <div class="mini-stat">
        <span class="mini-icon violet"><el-icon><UserFilled /></el-icon></span>
        <div>
          <strong>{{ users.length }}</strong>
          <span>本页用户</span>
        </div>
      </div>
    </div>

    <div class="section-card filter-card">
      <el-form :inline="true" class="filter-form" @submit.prevent="handleSearch">
        <el-form-item label="关键词">
          <el-input
            v-model="filters.keyword"
            placeholder="用户 ID / 账号 / 昵称"
            clearable
            style="width: 230px"
          >
            <template #prefix>
              <el-icon><Search /></el-icon>
            </template>
          </el-input>
        </el-form-item>
        <el-form-item label="账号状态">
          <el-select v-model="filters.status" style="width: 140px">
            <el-option label="全部状态" value="all" />
            <el-option label="正常" value="active" />
            <el-option label="已封禁" value="banned" />
          </el-select>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="handleSearch">
            <el-icon><Search /></el-icon>
            查询
          </el-button>
          <el-button @click="handleReset">
            <el-icon><RefreshLeft /></el-icon>
            重置
          </el-button>
        </el-form-item>
      </el-form>
    </div>

    <div class="section-card table-card">
      <div class="section-head">
        <div>
          <h3 class="section-title">用户列表</h3>
          <p class="section-desc">共 {{ pagination.total }} 条记录</p>
        </div>
        <el-tooltip content="数据来自 /admin/api/users" placement="top">
          <el-tag size="small" effect="plain" round>实时接口</el-tag>
        </el-tooltip>
      </div>

      <el-table v-loading="loading" :data="users" row-key="id" class="user-table">
        <el-table-column label="用户" min-width="230">
          <template #default="{ row }">
            <div class="user-cell">
              <div class="user-avatar" :class="`avatar-${row.id % 5}`">
                {{ (row.nickname || row.username).slice(0, 1) }}
              </div>
              <div class="user-meta">
                <strong>{{ row.nickname || row.username }}</strong>
                <span>@{{ row.username }} · ID {{ row.id }}</span>
              </div>
            </div>
          </template>
        </el-table-column>

        <el-table-column label="状态" width="110">
          <template #default="{ row }">
            <span class="status-pill" :class="row.status">
              <i />
              {{ row.status === 'active' ? '正常' : '已封禁' }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="好友数" width="100" align="center">
          <template #default="{ row }">
            <span class="num-cell">{{ row.friends }}</span>
          </template>
        </el-table-column>

        <el-table-column label="会话数" width="100" align="center">
          <template #default="{ row }">
            <span class="num-cell">{{ row.conversations }}</span>
          </template>
        </el-table-column>

        <el-table-column label="注册时间" width="170">
          <template #default="{ row }">
            <span class="time-cell">{{ row.createdAt }}</span>
          </template>
        </el-table-column>

        <el-table-column label="最近活跃" width="140">
          <template #default="{ row }">
            <span class="time-cell">{{ row.lastActive }}</span>
          </template>
        </el-table-column>

        <el-table-column label="操作" width="210" fixed="right">
          <template #default="{ row }">
            <div class="row-actions">
              <button class="text-btn primary" @click="openDetail(row)">详情</button>
              <button class="text-btn" :class="row.status === 'active' ? 'danger' : 'success'" @click="toggleStatus(row)">
                {{ row.status === 'active' ? '封禁' : '解禁' }}
              </button>
              <button class="text-btn" @click="resetPassword(row)">重置密码</button>
            </div>
          </template>
        </el-table-column>
      </el-table>

      <el-pagination
        v-model:current-page="pagination.page"
        v-model:page-size="pagination.pageSize"
        :page-sizes="[8, 12, 20]"
        :total="pagination.total"
        layout="total, sizes, prev, pager, next"
        background
        @size-change="handleSizeChange"
        @current-change="handleCurrentChange"
      />
    </div>

    <el-drawer v-model="drawerVisible" size="440px" :with-header="false" class="user-drawer">
      <div v-if="selectedUser" class="drawer-body">
        <div class="drawer-hero">
          <div class="drawer-avatar" :class="`avatar-${selectedUser.id % 5}`">
            {{ (selectedUser.nickname || selectedUser.username).slice(0, 1) }}
          </div>
          <div>
            <h3>{{ selectedUser.nickname || selectedUser.username }}</h3>
            <p>@{{ selectedUser.username }}</p>
            <span class="status-pill" :class="selectedUser.status">
              <i />
              {{ selectedUser.status === 'active' ? '正常' : '已封禁' }}
            </span>
          </div>
          <button class="drawer-close" type="button" @click="drawerVisible = false">
            <el-icon><Close /></el-icon>
          </button>
        </div>

        <div class="drawer-stats">
          <div>
            <strong>{{ selectedUser.friends }}</strong>
            <span>好友</span>
          </div>
          <div>
            <strong>{{ selectedUser.conversations }}</strong>
            <span>会话</span>
          </div>
          <div>
            <strong>{{ selectedUser.messages }}</strong>
            <span>消息</span>
          </div>
        </div>

        <div class="drawer-section">
          <h4>基础信息</h4>
          <dl class="info-list">
            <div><dt>用户 ID</dt><dd>{{ selectedUser.id }}</dd></div>
            <div><dt>登录账号</dt><dd>{{ selectedUser.username }}</dd></div>
            <div><dt>昵称</dt><dd>{{ selectedUser.nickname || selectedUser.username }}</dd></div>
            <div><dt>注册时间</dt><dd>{{ selectedUser.createdAt }}</dd></div>
            <div><dt>最近活跃</dt><dd>{{ selectedUser.lastActive }}</dd></div>
          </dl>
        </div>

        <div class="drawer-section">
          <h4>账号操作</h4>
          <div class="drawer-actions">
            <el-button
              :type="selectedUser.status === 'active' ? 'danger' : 'success'"
              plain
              @click="toggleStatus(selectedUser)"
            >
              {{ selectedUser.status === 'active' ? '封禁账号' : '解除封禁' }}
            </el-button>
            <el-button plain @click="resetPassword(selectedUser)">重置密码</el-button>
            <el-button plain @click="handleKick">强制下线</el-button>
          </div>
        </div>

        <p class="drawer-tip">
          <el-icon><InfoFilled /></el-icon>
          管理操作将记录到审计日志，请谨慎执行。
        </p>
      </div>
    </el-drawer>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  Search,
  RefreshLeft,
  Plus,
  Download,
  User,
  UserFilled,
  CircleCheck,
  CircleClose,
  Close,
  InfoFilled,
} from '@element-plus/icons-vue'
import {
  fetchUsers,
  updateUserStatus,
  type UserItem,
  type UserStatus,
} from '@/api/users'

const filters = reactive({
  keyword: '',
  status: 'all' as 'all' | UserStatus,
})

const pagination = reactive({
  page: 1,
  pageSize: 8,
  total: 0,
})

const users = ref<UserItem[]>([])
const loading = ref(false)
const drawerVisible = ref(false)
const selectedUser = ref<UserItem | null>(null)

const activeCount = computed(() => users.value.filter((user) => user.status === 'active').length)
const bannedCount = computed(() => users.value.filter((user) => user.status === 'banned').length)

async function loadUsers() {
  loading.value = true
  try {
    const data = await fetchUsers({
      page: pagination.page,
      page_size: pagination.pageSize,
      keyword: filters.keyword.trim(),
      status: filters.status,
    })
    users.value = data.list
    pagination.total = data.total
  } catch {
    users.value = []
    pagination.total = 0
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  pagination.page = 1
  loadUsers()
}

function handleReset() {
  filters.keyword = ''
  filters.status = 'all'
  pagination.page = 1
  loadUsers()
}

function handleSizeChange() {
  pagination.page = 1
  loadUsers()
}

function handleCurrentChange() {
  loadUsers()
}

function openDetail(user: UserItem) {
  selectedUser.value = user
  drawerVisible.value = true
}

async function toggleStatus(user: UserItem) {
  const nextStatus: UserStatus = user.status === 'active' ? 'banned' : 'active'
  const actionText = nextStatus === 'banned' ? '封禁' : '解禁'
  const name = user.nickname || user.username
  try {
    await ElMessageBox.confirm(
      `确定要${actionText}用户「${name}」吗？操作将记录到审计日志。`,
      `${actionText}确认`,
      {
        confirmButtonText: `确认${actionText}`,
        cancelButtonText: '取消',
        type: 'warning',
      },
    )
  } catch {
    return
  }

  try {
    await updateUserStatus(user.id, nextStatus)
    ElMessage.success(`已${actionText}用户「${name}」`)
    await loadUsers()
  } catch {
    // 请求拦截器已提示错误
  }
}

async function resetPassword(user: UserItem) {
  const name = user.nickname || user.username
  try {
    await ElMessageBox.confirm(
      `确定要重置用户「${name}」的登录密码吗？`,
      '重置密码',
      { confirmButtonText: '确认重置', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return
  }
  ElMessage.info('重置密码接口暂未实现')
}

function handleExport() {
  ElMessage.info('导出功能需接入后端并走审批流程')
}

function handleAdd() {
  ElMessage.info('新增用户需接入管理端 API')
}

function handleKick() {
  ElMessage.info('强制下线需 IM 服务提供在线状态接口')
}

onMounted(loadUsers)
</script>

<style scoped>
.header-actions {
  display: flex;
  gap: 10px;
}

.mini-stats {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 16px;
  margin-bottom: 20px;
}

.mini-stat {
  display: flex;
  align-items: center;
  gap: 13px;
  padding: 16px 18px;
  background: #fff;
  border: 1px solid var(--cx-border);
  border-radius: 14px;
  box-shadow: var(--cx-shadow-sm);
}

.mini-icon {
  width: 42px;
  height: 42px;
  border-radius: 12px;
  display: grid;
  place-items: center;
  font-size: 19px;
  flex-shrink: 0;
}

.mini-icon.blue {
  color: #2563eb;
  background: #eff6ff;
}

.mini-icon.emerald {
  color: #059669;
  background: #ecfdf5;
}

.mini-icon.rose {
  color: #e11d48;
  background: #fff1f2;
}

.mini-icon.violet {
  color: #7c3aed;
  background: #f5f3ff;
}

.mini-stat strong {
  display: block;
  font-size: 20px;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: var(--cx-ink);
}

.mini-stat span {
  font-size: 11px;
  color: #94a3b8;
}

.filter-card {
  padding: 18px 20px 4px;
  margin-bottom: 18px;
}

.filter-form :deep(.el-form-item) {
  margin-bottom: 14px;
}

.filter-form :deep(.el-form-item__label) {
  font-size: 12px;
  color: #64748b;
}

.table-card {
  padding: 20px 22px 22px;
}

.user-table {
  width: 100%;
}

.user-cell {
  display: flex;
  align-items: center;
  gap: 11px;
}

.user-avatar {
  width: 38px;
  height: 38px;
  border-radius: 11px;
  display: grid;
  place-items: center;
  font-size: 14px;
  font-weight: 700;
  color: #fff;
  flex-shrink: 0;
}

.avatar-0 {
  background: linear-gradient(135deg, #38bdf8, #2563eb);
}

.avatar-1 {
  background: linear-gradient(135deg, #a78bfa, #7c3aed);
}

.avatar-2 {
  background: linear-gradient(135deg, #34d399, #059669);
}

.avatar-3 {
  background: linear-gradient(135deg, #fbbf24, #f59e0b);
}

.avatar-4 {
  background: linear-gradient(135deg, #fb7185, #e11d48);
}

.user-meta {
  display: flex;
  flex-direction: column;
  line-height: 1.3;
  min-width: 0;
}

.user-meta strong {
  font-size: 13px;
  font-weight: 600;
  color: #1e293b;
}

.user-meta span {
  font-size: 11px;
  color: #94a3b8;
  margin-top: 2px;
}

.status-pill {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 4px 9px;
  border-radius: 999px;
  font-size: 11px;
  font-weight: 500;
}

.status-pill i {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.status-pill.active {
  color: #059669;
  background: #ecfdf5;
}

.status-pill.active i {
  background: #10b981;
}

.status-pill.banned {
  color: #e11d48;
  background: #fff1f2;
}

.status-pill.banned i {
  background: #f43f5e;
}

.num-cell {
  font-size: 13px;
  font-weight: 600;
  color: #475569;
}

.time-cell {
  font-size: 12px;
  color: #94a3b8;
}

.row-actions {
  display: flex;
  gap: 12px;
}

.text-btn {
  border: none;
  background: transparent;
  padding: 0;
  font-size: 12px;
  color: #64748b;
  cursor: pointer;
  transition: color 0.2s ease;
}

.text-btn:hover {
  color: #2563eb;
}

.text-btn.primary {
  color: #2563eb;
  font-weight: 500;
}

.text-btn.danger:hover {
  color: #e11d48;
}

.text-btn.success:hover {
  color: #059669;
}

/* Drawer */
.drawer-body {
  padding: 4px;
}

.drawer-hero {
  position: relative;
  display: flex;
  align-items: flex-start;
  gap: 14px;
  padding: 22px;
  border-radius: 16px;
  background:
    radial-gradient(circle at 90% 0%, rgba(56, 189, 248, 0.22), transparent 45%),
    linear-gradient(130deg, #0b1220, #0f2a5c);
  color: #e2e8f0;
  margin-bottom: 18px;
}

.drawer-avatar {
  width: 56px;
  height: 56px;
  border-radius: 16px;
  display: grid;
  place-items: center;
  font-size: 22px;
  font-weight: 700;
  color: #fff;
  flex-shrink: 0;
  box-shadow: 0 10px 22px rgba(2, 6, 23, 0.35);
}

.drawer-hero h3 {
  margin: 2px 0 3px;
  font-size: 18px;
  color: #fff;
}

.drawer-hero p {
  margin: 0 0 8px;
  font-size: 12px;
  color: #93c5fd;
}

.drawer-close {
  position: absolute;
  top: 14px;
  right: 14px;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: 8px;
  background: rgba(255, 255, 255, 0.1);
  color: #cbd5e1;
  cursor: pointer;
  display: grid;
  place-items: center;
}

.drawer-close:hover {
  background: rgba(255, 255, 255, 0.2);
}

.drawer-stats {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
  margin-bottom: 20px;
}

.drawer-stats div {
  text-align: center;
  padding: 14px 8px;
  border-radius: 12px;
  background: #f8fafc;
  border: 1px solid #eef2f8;
}

.drawer-stats strong {
  display: block;
  font-size: 19px;
  font-weight: 700;
  color: var(--cx-ink);
}

.drawer-stats span {
  font-size: 11px;
  color: #94a3b8;
}

.drawer-section {
  margin-bottom: 20px;
}

.drawer-section h4 {
  margin: 0 0 10px;
  font-size: 13px;
  font-weight: 600;
  color: #334155;
}

.info-list {
  margin: 0;
  padding: 14px 16px;
  border-radius: 12px;
  background: #f8fafc;
  border: 1px solid #eef2f8;
}

.info-list div {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 7px 0;
}

.info-list dt {
  font-size: 12px;
  color: #94a3b8;
}

.info-list dd {
  margin: 0;
  font-size: 12px;
  font-weight: 500;
  color: #334155;
  text-align: right;
}

.drawer-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}

.drawer-tip {
  display: flex;
  align-items: flex-start;
  gap: 6px;
  margin: 4px 0 0;
  padding: 12px 14px;
  border-radius: 10px;
  background: #fffbeb;
  border: 1px solid #fde68a;
  font-size: 11px;
  line-height: 1.6;
  color: #b45309;
}

@media (max-width: 1100px) {
  .mini-stats {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 640px) {
  .mini-stats {
    grid-template-columns: 1fr;
  }
  .header-actions {
    width: 100%;
  }
}
</style>
