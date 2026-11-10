<template>
  <div class="coming-page">
    <section class="coming-hero">
      <div class="hero-orb orb-a" />
      <div class="hero-orb orb-b" />
      <div class="hero-grid" />

      <div class="coming-copy">
        <span class="coming-badge">
          <i />
          MODULE PREVIEW
        </span>
        <div class="coming-icon">
          <el-icon :size="30"><MagicStick /></el-icon>
        </div>
        <h1>{{ title }}</h1>
        <p class="coming-desc">{{ description }}</p>

        <div class="coming-actions">
          <el-button type="primary" size="large" @click="goDashboard">
            <el-icon><Odometer /></el-icon>
            返回工作台
          </el-button>
          <el-button size="large" plain @click="notify">
            <el-icon><Bell /></el-icon>
            订阅上线通知
          </el-button>
        </div>
      </div>

      <div class="coming-preview">
        <div class="preview-head">
          <span class="preview-dots"><i /><i /><i /></span>
          <span>{{ title }} · 规划中</span>
        </div>
        <ul class="feature-list">
          <li v-for="(feature, index) in features" :key="feature">
            <span class="feature-index">{{ String(index + 1).padStart(2, '0') }}</span>
            <span class="feature-name">{{ feature }}</span>
            <el-tag size="small" effect="plain" round>待开发</el-tag>
          </li>
        </ul>
        <div class="preview-foot">
          <el-icon><InfoFilled /></el-icon>
          该模块依赖后端能力就绪，详见《Admin-Requirement.md》分期计划
        </div>
      </div>
    </section>

    <section class="plan-grid">
      <article class="plan-card done">
        <div class="plan-step">M0</div>
        <h3>基础框架</h3>
        <p>登录、主布局、路由与权限骨架已就绪。</p>
        <span class="plan-status"><el-icon><CircleCheck /></el-icon>已完成</span>
      </article>
      <article class="plan-card active">
        <div class="plan-step">M1</div>
        <h3>{{ title }}</h3>
        <p>界面骨架与交互设计已完成，等待后端 API 联调。</p>
        <span class="plan-status"><el-icon><Loading /></el-icon>进行中</span>
      </article>
      <article class="plan-card">
        <div class="plan-step">M2</div>
        <h3>联调与验收</h3>
        <p>接入真实数据、权限校验、审计留痕与验收测试。</p>
        <span class="plan-status"><el-icon><Timer /></el-icon>待开始</span>
      </article>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import {
  MagicStick,
  Odometer,
  Bell,
  InfoFilled,
  CircleCheck,
  Loading,
  Timer,
} from '@element-plus/icons-vue'

const route = useRoute()
const router = useRouter()

const title = computed(() => (route.meta.title as string) || '模块建设中')
const description = computed(
  () => (route.meta.description as string) || '该模块正在规划中，敬请期待。',
)
const features = computed(() => (route.meta.features as string[] | undefined) || [])

function goDashboard() {
  router.push('/dashboard')
}

function notify() {
  ElMessage.success('已记录订阅，模块上线后将通知你')
}
</script>

<style scoped>
.coming-hero {
  position: relative;
  display: grid;
  grid-template-columns: minmax(0, 1.05fr) minmax(320px, 0.95fr);
  align-items: center;
  gap: 40px;
  padding: 46px 48px;
  border-radius: 22px;
  background:
    radial-gradient(circle at 82% 12%, rgba(56, 189, 248, 0.26), transparent 40%),
    linear-gradient(140deg, #070d1a 0%, #0b1830 50%, #0f2a5c 100%);
  box-shadow: 0 24px 60px rgba(15, 23, 42, 0.18);
  overflow: hidden;
  margin-bottom: 22px;
}

.hero-grid {
  position: absolute;
  inset: 0;
  background-image:
    linear-gradient(rgba(148, 163, 184, 0.06) 1px, transparent 1px),
    linear-gradient(90deg, rgba(148, 163, 184, 0.06) 1px, transparent 1px);
  background-size: 52px 52px;
  mask-image: radial-gradient(circle at 60% 40%, #000 20%, transparent 76%);
  pointer-events: none;
}

.hero-orb {
  position: absolute;
  border-radius: 50%;
  pointer-events: none;
}

.orb-a {
  width: 300px;
  height: 300px;
  right: -80px;
  top: -120px;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.4), transparent 68%);
}

.orb-b {
  width: 220px;
  height: 220px;
  left: -70px;
  bottom: -110px;
  background: radial-gradient(circle, rgba(6, 182, 212, 0.3), transparent 68%);
}

.coming-copy {
  position: relative;
  z-index: 1;
  color: #e2e8f0;
}

.coming-badge {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 5px 11px;
  border-radius: 999px;
  border: 1px solid rgba(56, 189, 248, 0.24);
  background: rgba(56, 189, 248, 0.08);
  font-size: 10px;
  letter-spacing: 0.16em;
  color: #7dd3fc;
}

.coming-badge i {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #38bdf8;
  box-shadow: 0 0 8px #38bdf8;
}

.coming-icon {
  width: 62px;
  height: 62px;
  margin: 24px 0 18px;
  border-radius: 18px;
  display: grid;
  place-items: center;
  color: #fff;
  background: linear-gradient(135deg, #38bdf8, #2563eb 60%, #7c3aed);
  box-shadow: 0 16px 34px rgba(37, 99, 235, 0.42);
}

.coming-copy h1 {
  margin: 0 0 12px;
  font-size: 34px;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: #fff;
}

.coming-desc {
  margin: 0 0 26px;
  max-width: 460px;
  font-size: 14px;
  line-height: 1.85;
  color: #94a3b8;
}

.coming-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}

.coming-actions :deep(.el-button) {
  border-radius: 11px;
}

.coming-actions :deep(.el-button--primary) {
  border: none;
  background: linear-gradient(95deg, #2563eb, #3b82f6 55%, #6366f1);
  box-shadow: 0 12px 28px rgba(37, 99, 235, 0.35);
}

.coming-actions :deep(.el-button.is-plain) {
  background: rgba(255, 255, 255, 0.06);
  border-color: rgba(191, 219, 254, 0.28);
  color: #e2e8f0;
}

.coming-actions :deep(.el-button.is-plain:hover) {
  background: rgba(255, 255, 255, 0.14);
  border-color: rgba(191, 219, 254, 0.5);
  color: #fff;
}

.coming-preview {
  position: relative;
  z-index: 1;
  padding: 20px;
  border-radius: 18px;
  background: linear-gradient(160deg, rgba(30, 41, 59, 0.72), rgba(15, 23, 42, 0.62));
  border: 1px solid rgba(148, 163, 184, 0.18);
  box-shadow: 0 24px 50px rgba(2, 6, 23, 0.35);
  backdrop-filter: blur(12px);
}

.preview-head {
  display: flex;
  align-items: center;
  gap: 12px;
  padding-bottom: 14px;
  border-bottom: 1px dashed rgba(148, 163, 184, 0.16);
  font-size: 12px;
  color: #cbd5e1;
}

.preview-dots {
  display: inline-flex;
  gap: 5px;
}

.preview-dots i {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: rgba(148, 163, 184, 0.35);
}

.preview-dots i:first-child {
  background: #f87171;
}

.preview-dots i:nth-child(2) {
  background: #fbbf24;
}

.preview-dots i:nth-child(3) {
  background: #34d399;
}

.feature-list {
  list-style: none;
  margin: 0;
  padding: 8px 0;
}

.feature-list li {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 11px 0;
  border-bottom: 1px dashed rgba(148, 163, 184, 0.12);
}

.feature-list li:last-child {
  border-bottom: none;
}

.feature-index {
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.06em;
  color: #475569;
}

.feature-name {
  font-size: 13px;
  color: #e2e8f0;
  flex: 1;
}

.preview-foot {
  display: flex;
  align-items: flex-start;
  gap: 6px;
  margin-top: 10px;
  padding-top: 14px;
  border-top: 1px dashed rgba(148, 163, 184, 0.16);
  font-size: 10px;
  line-height: 1.6;
  color: #64748b;
}

.plan-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 18px;
}

.plan-card {
  position: relative;
  padding: 22px;
  background: #fff;
  border: 1px solid var(--cx-border);
  border-radius: 16px;
  box-shadow: var(--cx-shadow-sm);
  overflow: hidden;
}

.plan-card::before {
  content: '';
  position: absolute;
  inset: 0 0 auto 0;
  height: 3px;
  background: #e2e8f0;
}

.plan-card.done::before {
  background: linear-gradient(90deg, #34d399, #10b981);
}

.plan-card.active::before {
  background: linear-gradient(90deg, #38bdf8, #2563eb);
}

.plan-step {
  display: inline-block;
  margin-bottom: 12px;
  padding: 3px 9px;
  border-radius: 6px;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.1em;
  color: #64748b;
  background: #f1f5f9;
}

.plan-card.done .plan-step {
  color: #059669;
  background: #ecfdf5;
}

.plan-card.active .plan-step {
  color: #2563eb;
  background: #eff6ff;
}

.plan-card h3 {
  margin: 0 0 6px;
  font-size: 15px;
  font-weight: 600;
  color: var(--cx-ink);
}

.plan-card p {
  margin: 0 0 14px;
  font-size: 12px;
  line-height: 1.7;
  color: #94a3b8;
}

.plan-status {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  color: #94a3b8;
}

.plan-card.done .plan-status {
  color: #059669;
}

.plan-card.active .plan-status {
  color: #2563eb;
}

@media (max-width: 1080px) {
  .coming-hero {
    grid-template-columns: 1fr;
    padding: 34px;
  }
  .plan-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 640px) {
  .coming-hero {
    padding: 26px 22px;
  }
  .coming-copy h1 {
    font-size: 26px;
  }
}
</style>
