<template>
  <div class="dashboard">
    <section class="welcome-card">
      <div class="welcome-glow" />
      <div class="welcome-copy">
        <p class="welcome-eyebrow">{{ dateText }}</p>
        <h1>{{ greeting }}，{{ auth.displayName }}</h1>
        <p class="welcome-desc">这是《铖信》今日的运行概览，一切尽在掌握。</p>
        <div class="welcome-tags">
          <span><b class="dot green" />管理接口已连接</span>
          <span><b class="dot blue" />数据来自实时接口</span>
          <span><b class="dot violet" />审计日志已开启</span>
        </div>
      </div>
      <div class="welcome-side">
        <div class="welcome-kpi">
          <span>当前在线</span>
          <strong>{{ onlineDisplay }}</strong>
          <em>{{ onlineHint }}</em>
        </div>
        <el-button class="refresh-btn" :loading="refreshing" @click="handleRefresh">
          <el-icon><Refresh /></el-icon>
          刷新数据
        </el-button>
      </div>
    </section>

    <section class="stat-grid">
      <article v-for="item in stats" :key="item.label" class="stat-card" :class="`tone-${item.tone}`">
        <div class="stat-top">
          <div class="stat-icon">
            <el-icon :size="22"><component :is="item.icon" /></el-icon>
          </div>
          <span v-if="item.delta" class="stat-delta" :class="{ down: item.up === false }">
            {{ item.delta }}
          </span>
        </div>
        <p class="stat-label">{{ item.label }}</p>
        <div class="stat-bottom">
          <strong class="stat-value">{{ item.value }}</strong>
          <div v-if="item.spark.length" class="spark">
            <i
              v-for="(bar, index) in item.spark"
              :key="index"
              :style="{ height: `${bar}%` }"
            />
          </div>
        </div>
        <p class="stat-foot">{{ item.foot }}</p>
      </article>
    </section>

    <section class="chart-grid">
      <article class="section-card trend-card">
        <header class="section-head">
          <div>
            <h3 class="section-title">消息与活跃趋势</h3>
            <p class="section-desc">消息量、日活跃用户变化情况</p>
          </div>
          <el-radio-group v-model="range" size="small">
            <el-radio-button value="7">近 7 天</el-radio-button>
            <el-radio-button value="30">近 30 天</el-radio-button>
          </el-radio-group>
        </header>
        <div ref="trendEl" class="chart chart-trend" />
      </article>

      <article class="section-card donut-card">
        <header class="section-head">
          <div>
            <h3 class="section-title">消息类型分布</h3>
            <p class="section-desc">示例占比 · 类型统计接口待接入</p>
          </div>
          <el-tag size="small" effect="plain" round>示例</el-tag>
        </header>
        <div class="donut-wrap">
          <div ref="donutEl" class="chart chart-donut" />
          <div class="donut-center">
            <strong>{{ todayMessagesShort }}</strong>
            <span>今日消息</span>
          </div>
        </div>
        <ul class="legend-list">
          <li v-for="item in messageTypes" :key="item.name">
            <span class="legend-dot" :style="{ background: item.color }" />
            <span class="legend-name">{{ item.name }}</span>
            <span class="legend-value">{{ item.value }}%</span>
          </li>
        </ul>
      </article>
    </section>

    <section class="bottom-grid">
      <article class="section-card">
        <header class="section-head">
          <div>
            <h3 class="section-title">系统状态</h3>
            <p class="section-desc">示例状态 · 监控接口待接入</p>
          </div>
          <el-tag size="small" type="info" effect="light" round>示例</el-tag>
        </header>
        <ul class="status-list">
          <li v-for="item in systemStatus" :key="item.name" class="status-item">
            <div class="status-icon" :class="`s-${item.state}`">
              <el-icon :size="18"><component :is="item.icon" /></el-icon>
            </div>
            <div class="status-copy">
              <strong>{{ item.name }}</strong>
              <span>{{ item.desc }}</span>
            </div>
            <div class="status-latency">
              <b :class="`s-${item.state}`">{{ item.latency }}</b>
              <span>{{ item.stateText }}</span>
            </div>
          </li>
        </ul>
      </article>

      <article class="section-card">
        <header class="section-head">
          <div>
            <h3 class="section-title">最近操作</h3>
            <p class="section-desc">示例数据 · 审计查询接口待接入</p>
          </div>
          <router-link to="/audit" class="more-link">
            查看全部
            <el-icon :size="12"><ArrowRight /></el-icon>
          </router-link>
        </header>
        <ul class="activity-list">
          <li v-for="item in activities" :key="item.id" class="activity-item">
            <div class="activity-avatar" :class="`tone-${item.tone}`">{{ item.name.slice(0, 1) }}</div>
            <div class="activity-copy">
              <p>
                <strong>{{ item.name }}</strong>
                {{ item.action }}
                <b>{{ item.target }}</b>
              </p>
              <span>{{ item.time }}</span>
            </div>
            <el-tag size="small" :type="item.tagType" effect="light" round>{{ item.tag }}</el-tag>
          </li>
        </ul>
      </article>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import type { Component } from 'vue'
import * as echarts from 'echarts'
import { ElMessage } from 'element-plus'
import {
  UserFilled,
  ChatLineSquare,
  TrendCharts,
  Plus,
  Refresh,
  ArrowRight,
  Connection,
  Coin,
  Promotion,
  FolderOpened,
} from '@element-plus/icons-vue'
import { useAuthStore } from '@/stores/auth'
import {
  fetchStatsOverview,
  fetchStatsTrend,
  type StatsOverview,
  type TrendData,
} from '@/api/stats'

const auth = useAuthStore()

const range = ref<'7' | '30'>('7')
const refreshing = ref(false)
const overview = ref<StatsOverview | null>(null)
const trend = ref<TrendData>({ dates: [], messages: [], actives: [] })

interface StatCard {
  label: string
  value: string
  delta: string
  up: boolean
  icon: Component
  tone: string
  spark: number[]
  foot: string
}

function formatNumber(value?: number | null): string {
  if (value === undefined || value === null) {
    return '—'
  }
  return Number(value).toLocaleString('zh-CN')
}

const onlineDisplay = computed(() => {
  const data = overview.value
  if (!data?.onlineUsersAvailable) {
    return '—'
  }
  return formatNumber(data.onlineUsers)
})

const onlineHint = computed(() =>
  overview.value?.onlineUsersAvailable ? '实时在线用户' : '在线状态未接入',
)

const todayMessagesShort = computed(() => {
  const value = overview.value?.todayMessages
  if (value === undefined || value === null) {
    return '—'
  }
  if (value >= 1000) {
    return `${(value / 1000).toFixed(1)}k`
  }
  return String(value)
})

const stats = computed<StatCard[]>(() => {
  const data = overview.value
  const onlineAvailable = Boolean(data?.onlineUsersAvailable)
  return [
    {
      label: '当前在线用户',
      value: onlineAvailable ? formatNumber(data?.onlineUsers) : '—',
      delta: onlineAvailable ? '实时' : '',
      up: true,
      icon: UserFilled,
      tone: 'blue',
      spark: [],
      foot: onlineAvailable ? '来自 IM 在线状态' : '在线状态未接入',
    },
    {
      label: '今日消息量',
      value: formatNumber(data?.todayMessages),
      delta: '今日',
      up: true,
      icon: ChatLineSquare,
      tone: 'violet',
      spark: [],
      foot: '今日消息总数',
    },
    {
      label: '日活跃用户',
      value: formatNumber(data?.activeUsers),
      delta: '今日',
      up: true,
      icon: TrendCharts,
      tone: 'emerald',
      spark: [],
      foot: '今日有发消息的用户',
    },
    {
      label: '今日新增用户',
      value: formatNumber(data?.newUsers),
      delta: '今日',
      up: true,
      icon: Plus,
      tone: 'amber',
      spark: [],
      foot: '今日新注册用户',
    },
  ]
})

const messageTypes = [
  { name: '文本消息', value: 68, color: '#2563eb' },
  { name: '图片消息', value: 18, color: '#7c3aed' },
  { name: '语音消息', value: 6, color: '#06b6d4' },
  { name: '文件消息', value: 5, color: '#f59e0b' },
  { name: '其他类型', value: 3, color: '#cbd5e1' },
]

const systemStatus = [
  {
    name: 'IM 长连接服务',
    desc: 'TCP 8888 · 在线状态未接入',
    latency: '—',
    state: 'warn',
    stateText: '待接入',
    icon: Connection,
  },
  {
    name: 'MariaDB 数据库',
    desc: '连接池 6 / 8 · 主库',
    latency: '8ms',
    state: 'ok',
    stateText: '正常',
    icon: Coin,
  },
  {
    name: '推送服务',
    desc: 'FCM / APNs 通道',
    latency: '维护中',
    state: 'warn',
    stateText: '待接入',
    icon: Promotion,
  },
  {
    name: '对象存储',
    desc: '图片 / 语音 / 文件',
    latency: '—',
    state: 'off',
    stateText: '未配置',
    icon: FolderOpened,
  },
]

const activities = [
  {
    id: 1,
    name: '超级管理员',
    action: '封禁了用户',
    target: 'u_10293',
    time: '2 分钟前',
    tag: '用户管理',
    tagType: 'danger',
    tone: 'blue',
  },
  {
    id: 2,
    name: '审核员 小林',
    action: '处理了一条举报',
    target: '#R-2048',
    time: '18 分钟前',
    tag: '内容审核',
    tagType: 'warning',
    tone: 'violet',
  },
  {
    id: 3,
    name: '系统管理员',
    action: '更新了限流配置',
    target: 'msg_rate',
    time: '42 分钟前',
    tag: '系统配置',
    tagType: 'info',
    tone: 'emerald',
  },
  {
    id: 4,
    name: '运营 小周',
    action: '创建了推送任务',
    target: '双十一公告',
    time: '1 小时前',
    tag: '推送管理',
    tagType: 'success',
    tone: 'amber',
  },
]

const today = new Date()
const weekMap = ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六']
const dateText = `${today.getFullYear()} 年 ${today.getMonth() + 1} 月 ${today.getDate()} 日 · ${weekMap[today.getDay()]}`

const greeting = computed(() => {
  const hour = new Date().getHours()
  if (hour < 6) return '夜深了'
  if (hour < 12) return '早上好'
  if (hour < 18) return '下午好'
  return '晚上好'
})

const trendEl = ref<HTMLElement>()
const donutEl = ref<HTMLElement>()
let trendChart: echarts.ECharts | null = null
let donutChart: echarts.ECharts | null = null

function renderTrend() {
  if (!trendChart) return
  const data = trend.value
  const option: echarts.EChartsOption = {
    tooltip: {
      trigger: 'axis',
      backgroundColor: 'rgba(15, 23, 42, 0.92)',
      borderWidth: 0,
      padding: [10, 14],
      textStyle: { color: '#e2e8f0', fontSize: 12 },
      axisPointer: {
        type: 'line',
        lineStyle: { color: 'rgba(148, 163, 184, 0.4)', type: 'dashed' },
      },
    },
    legend: {
      data: ['消息量', '活跃用户'],
      right: 0,
      top: 0,
      icon: 'circle',
      itemWidth: 8,
      itemHeight: 8,
      itemGap: 18,
      textStyle: { color: '#64748b', fontSize: 12 },
    },
    grid: { left: 6, right: 10, top: 46, bottom: 6, containLabel: true },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: data.dates,
      axisLine: { lineStyle: { color: '#e8edf5' } },
      axisTick: { show: false },
      axisLabel: { color: '#94a3b8', fontSize: 11, interval: range.value === '30' ? 4 : 0 },
    },
    yAxis: {
      type: 'value',
      splitLine: { lineStyle: { color: '#f1f5f9' } },
      axisLabel: { color: '#94a3b8', fontSize: 11 },
    },
    series: [
      {
        name: '消息量',
        type: 'line',
        smooth: true,
        showSymbol: false,
        symbol: 'circle',
        symbolSize: 7,
        data: data.messages,
        lineStyle: { width: 3, color: '#2563eb' },
        itemStyle: { color: '#2563eb' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(37, 99, 235, 0.26)' },
            { offset: 1, color: 'rgba(37, 99, 235, 0)' },
          ]),
        },
      },
      {
        name: '活跃用户',
        type: 'line',
        smooth: true,
        showSymbol: false,
        symbol: 'circle',
        symbolSize: 7,
        data: data.actives,
        lineStyle: { width: 2.5, color: '#7c3aed' },
        itemStyle: { color: '#7c3aed' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(124, 58, 237, 0.16)' },
            { offset: 1, color: 'rgba(124, 58, 237, 0)' },
          ]),
        },
      },
    ],
  }
  trendChart.setOption(option, true)
}

function renderDonut() {
  if (!donutChart) return
  const option: echarts.EChartsOption = {
    tooltip: {
      trigger: 'item',
      backgroundColor: 'rgba(15, 23, 42, 0.92)',
      borderWidth: 0,
      textStyle: { color: '#e2e8f0', fontSize: 12 },
      formatter: '{b}：{c}%',
    },
    series: [
      {
        type: 'pie',
        radius: ['62%', '86%'],
        center: ['50%', '50%'],
        avoidLabelOverlap: true,
        itemStyle: {
          borderColor: '#fff',
          borderWidth: 4,
          borderRadius: 8,
        },
        label: { show: false },
        labelLine: { show: false },
        emphasis: {
          scale: true,
          scaleSize: 6,
        },
        data: messageTypes.map((item) => ({
          value: item.value,
          name: item.name,
          itemStyle: { color: item.color },
        })),
      },
    ],
  }
  donutChart.setOption(option, true)
}

function initCharts() {
  if (trendEl.value && !trendChart) {
    trendChart = echarts.init(trendEl.value)
  }
  if (donutEl.value && !donutChart) {
    donutChart = echarts.init(donutEl.value)
  }
  renderTrend()
  renderDonut()
}

function handleResize() {
  trendChart?.resize()
  donutChart?.resize()
}

async function loadOverview(): Promise<boolean> {
  try {
    overview.value = await fetchStatsOverview()
    return true
  } catch {
    overview.value = null
    return false
  }
}

async function loadTrend(): Promise<boolean> {
  let ok = true
  try {
    trend.value = await fetchStatsTrend(Number(range.value) as 7 | 30)
  } catch {
    trend.value = { dates: [], messages: [], actives: [] }
    ok = false
  }
  renderTrend()
  return ok
}

async function handleRefresh() {
  refreshing.value = true
  try {
    const results = await Promise.all([loadOverview(), loadTrend()])
    if (results.every(Boolean)) {
      ElMessage.success('数据已刷新')
    } else {
      ElMessage.warning('部分数据刷新失败')
    }
  } finally {
    refreshing.value = false
  }
}

watch(range, () => {
  loadTrend()
})

onMounted(async () => {
  await nextTick()
  initCharts()
  await Promise.all([loadOverview(), loadTrend()])
  window.addEventListener('resize', handleResize)
})

onBeforeUnmount(() => {
  window.removeEventListener('resize', handleResize)
  trendChart?.dispose()
  donutChart?.dispose()
  trendChart = null
  donutChart = null
})
</script>

<style scoped>
.welcome-card {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 28px;
  padding: 30px 34px;
  margin-bottom: 22px;
  border-radius: 20px;
  color: #e2e8f0;
  background:
    radial-gradient(circle at 88% 20%, rgba(56, 189, 248, 0.34), transparent 40%),
    linear-gradient(120deg, #0b1220 0%, #0f2a5c 62%, #1d4ed8 130%);
  box-shadow: 0 20px 44px rgba(15, 23, 42, 0.16);
  overflow: hidden;
}

.welcome-glow {
  position: absolute;
  width: 360px;
  height: 360px;
  right: -120px;
  bottom: -220px;
  border-radius: 50%;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.5), transparent 68%);
  pointer-events: none;
}

.welcome-copy {
  position: relative;
  z-index: 1;
}

.welcome-eyebrow {
  margin: 0 0 10px;
  font-size: 11px;
  letter-spacing: 0.12em;
  color: #93c5fd;
}

.welcome-copy h1 {
  margin: 0 0 8px;
  font-size: 26px;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: #fff;
}

.welcome-desc {
  margin: 0;
  font-size: 13px;
  color: #94a3b8;
}

.welcome-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
  margin-top: 16px;
  font-size: 11px;
  color: #cbd5e1;
}

.welcome-tags span {
  display: inline-flex;
  align-items: center;
  gap: 6px;
}

.welcome-tags .dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.dot.green {
  background: #22c55e;
  box-shadow: 0 0 8px rgba(34, 197, 94, 0.7);
}

.dot.blue {
  background: #38bdf8;
  box-shadow: 0 0 8px rgba(56, 189, 248, 0.7);
}

.dot.violet {
  background: #a78bfa;
  box-shadow: 0 0 8px rgba(167, 139, 250, 0.7);
}

.welcome-side {
  position: relative;
  z-index: 1;
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: 14px;
  flex-shrink: 0;
}

.welcome-kpi {
  text-align: right;
}

.welcome-kpi span {
  display: block;
  font-size: 11px;
  color: #93c5fd;
  margin-bottom: 2px;
}

.welcome-kpi strong {
  font-size: 34px;
  font-weight: 700;
  letter-spacing: -0.03em;
  color: #fff;
}

.welcome-kpi em {
  display: block;
  margin-top: 2px;
  font-size: 11px;
  font-style: normal;
  color: #86efac;
}

.refresh-btn {
  border: 1px solid rgba(191, 219, 254, 0.28);
  background: rgba(255, 255, 255, 0.12);
  color: #e2e8f0;
}

.refresh-btn:hover {
  background: rgba(255, 255, 255, 0.2);
  color: #fff;
}

.stat-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 18px;
  margin-bottom: 22px;
}

.stat-card {
  position: relative;
  padding: 20px 20px 16px;
  background: #fff;
  border: 1px solid var(--cx-border);
  border-radius: 16px;
  box-shadow: var(--cx-shadow-sm);
  overflow: hidden;
  transition: transform 0.22s ease, box-shadow 0.22s ease;
}

.stat-card::after {
  content: '';
  position: absolute;
  inset: 0 0 auto 0;
  height: 3px;
  background: var(--tone);
  opacity: 0.9;
}

.stat-card:hover {
  transform: translateY(-4px);
  box-shadow: var(--cx-shadow);
}

.tone-blue {
  --tone: linear-gradient(90deg, #38bdf8, #2563eb);
  --tone-soft: #eff6ff;
  --tone-ink: #2563eb;
}

.tone-violet {
  --tone: linear-gradient(90deg, #a78bfa, #7c3aed);
  --tone-soft: #f5f3ff;
  --tone-ink: #7c3aed;
}

.tone-emerald {
  --tone: linear-gradient(90deg, #34d399, #10b981);
  --tone-soft: #ecfdf5;
  --tone-ink: #059669;
}

.tone-amber {
  --tone: linear-gradient(90deg, #fbbf24, #f59e0b);
  --tone-soft: #fffbeb;
  --tone-ink: #d97706;
}

.stat-top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 16px;
}

.stat-icon {
  width: 44px;
  height: 44px;
  border-radius: 13px;
  display: grid;
  place-items: center;
  color: var(--tone-ink);
  background: var(--tone-soft);
}

.stat-delta {
  font-size: 12px;
  font-weight: 600;
  color: #059669;
  padding: 3px 8px;
  border-radius: 999px;
  background: #ecfdf5;
}

.stat-delta.down {
  color: #e11d48;
  background: #fff1f2;
}

.stat-label {
  margin: 0 0 8px;
  font-size: 12px;
  color: #64748b;
}

.stat-bottom {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 12px;
}

.stat-value {
  font-size: 28px;
  font-weight: 700;
  letter-spacing: -0.03em;
  color: var(--cx-ink);
  line-height: 1;
}

.spark {
  display: flex;
  align-items: flex-end;
  gap: 3px;
  height: 30px;
  flex-shrink: 0;
}

.spark i {
  width: 4px;
  border-radius: 2px;
  background: var(--tone);
  opacity: 0.55;
}

.stat-foot {
  margin: 12px 0 0;
  padding-top: 10px;
  border-top: 1px dashed #eef2f8;
  font-size: 11px;
  color: #94a3b8;
}

.chart-grid {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(300px, 1fr);
  gap: 18px;
  margin-bottom: 22px;
}

.chart {
  width: 100%;
}

.chart-trend {
  height: 322px;
}

.donut-wrap {
  position: relative;
}

.chart-donut {
  height: 220px;
}

.donut-center {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  pointer-events: none;
}

.donut-center strong {
  font-size: 26px;
  font-weight: 700;
  letter-spacing: -0.03em;
  color: var(--cx-ink);
}

.donut-center span {
  margin-top: 2px;
  font-size: 11px;
  color: #94a3b8;
}

.legend-list {
  list-style: none;
  margin: 16px 0 0;
  padding: 0;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px 16px;
}

.legend-list li {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
}

.legend-dot {
  width: 8px;
  height: 8px;
  border-radius: 3px;
  flex-shrink: 0;
}

.legend-name {
  color: #64748b;
}

.legend-value {
  margin-left: auto;
  font-weight: 600;
  color: #334155;
}

.bottom-grid {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1.15fr);
  gap: 18px;
}

.status-list,
.activity-list {
  list-style: none;
  margin: 0;
  padding: 0;
}

.status-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 0;
  border-bottom: 1px dashed #eef2f8;
}

.status-item:last-child {
  border-bottom: none;
  padding-bottom: 0;
}

.status-icon {
  width: 38px;
  height: 38px;
  border-radius: 11px;
  display: grid;
  place-items: center;
  flex-shrink: 0;
}

.status-icon.s-ok {
  background: #ecfdf5;
  color: #059669;
}

.status-icon.s-warn {
  background: #fffbeb;
  color: #d97706;
}

.status-icon.s-off {
  background: #f1f5f9;
  color: #94a3b8;
}

.status-copy {
  min-width: 0;
}

.status-copy strong {
  display: block;
  font-size: 13px;
  font-weight: 600;
  color: #1e293b;
}

.status-copy span {
  font-size: 11px;
  color: #94a3b8;
}

.status-latency {
  margin-left: auto;
  text-align: right;
  flex-shrink: 0;
}

.status-latency b {
  display: block;
  font-size: 12px;
  font-weight: 600;
}

.status-latency b.s-ok {
  color: #059669;
}

.status-latency b.s-warn {
  color: #d97706;
}

.status-latency b.s-off {
  color: #94a3b8;
}

.status-latency span {
  font-size: 10px;
  color: #cbd5e1;
}

.activity-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 11px 0;
  border-bottom: 1px dashed #eef2f8;
}

.activity-item:last-child {
  border-bottom: none;
  padding-bottom: 0;
}

.activity-avatar {
  width: 36px;
  height: 36px;
  border-radius: 11px;
  display: grid;
  place-items: center;
  font-size: 13px;
  font-weight: 700;
  color: #fff;
  flex-shrink: 0;
  background: var(--tone);
}

.activity-copy {
  min-width: 0;
  flex: 1;
}

.activity-copy p {
  margin: 0;
  font-size: 12px;
  color: #64748b;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.activity-copy p strong {
  color: #1e293b;
  font-weight: 600;
  margin-right: 2px;
}

.activity-copy p b {
  color: #2563eb;
  font-weight: 500;
}

.activity-copy span {
  font-size: 10px;
  color: #b6c2d2;
}

.more-link {
  display: inline-flex;
  align-items: center;
  gap: 3px;
  font-size: 12px;
  color: #2563eb;
}

.more-link:hover {
  color: #1d4ed8;
}

@media (max-width: 1280px) {
  .stat-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 1080px) {
  .chart-grid,
  .bottom-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 720px) {
  .welcome-card {
    flex-direction: column;
    align-items: flex-start;
    padding: 24px;
  }
  .welcome-side {
    align-items: flex-start;
    text-align: left;
  }
  .welcome-kpi {
    text-align: left;
  }
  .stat-grid {
    grid-template-columns: 1fr;
  }
}
</style>
