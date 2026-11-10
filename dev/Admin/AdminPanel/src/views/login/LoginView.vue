<template>
  <div class="login-page">
    <section class="login-hero">
      <div class="hero-grid" />
      <div class="orb orb-1" />
      <div class="orb orb-2" />
      <div class="orb orb-3" />

      <div class="hero-brand">
        <div class="brand-mark">铖</div>
        <div class="brand-copy">
          <strong>铖信</strong>
          <span>KC-CXIM · ADMIN CONSOLE</span>
        </div>
      </div>

      <div class="hero-main">
        <p class="hero-eyebrow">
          <span class="dot" />
          IM OPERATIONS PLATFORM
        </p>
        <h1 class="hero-title">
          让每一次沟通<br />
          <em>安全、清晰、可掌控</em>
        </h1>
        <p class="hero-desc">
          面向运营与管理员的一体化控制台，统一管理用户、群组、消息与内容，
          实时掌握《铖信》的运行脉搏。
        </p>

        <div class="hero-features">
          <div class="feature-chip">
            <el-icon><User /></el-icon>
            用户与关系链
          </div>
          <div class="feature-chip">
            <el-icon><ChatDotRound /></el-icon>
            会话与消息治理
          </div>
          <div class="feature-chip">
            <el-icon><Warning /></el-icon>
            内容安全审核
          </div>
          <div class="feature-chip">
            <el-icon><DataAnalysis /></el-icon>
            运营数据洞察
          </div>
        </div>
      </div>

      <div class="hero-preview">
        <div class="preview-top">
          <span class="preview-dots"><i /><i /><i /></span>
          <span class="preview-label">实时运行概览</span>
          <span class="preview-live"><b />LIVE</span>
        </div>
        <div class="preview-stats">
          <div class="preview-stat">
            <span>在线用户</span>
            <strong>1,284</strong>
            <em class="up">+12.6%</em>
          </div>
          <div class="preview-stat">
            <span>今日消息</span>
            <strong>48,920</strong>
            <em class="up">+8.2%</em>
          </div>
          <div class="preview-stat">
            <span>活跃率</span>
            <strong>73.4%</strong>
            <em class="up">+3.1%</em>
          </div>
        </div>
        <div class="preview-chart">
          <div v-for="(bar, index) in previewBars" :key="index" class="preview-bar" :style="{ height: `${bar}%` }" />
        </div>
        <div class="preview-foot">
          <span><b class="green" />长连接正常</span>
          <span><b class="blue" />数据库 8ms</span>
          <span><b class="violet" />推送队列空闲</span>
        </div>
      </div>

      <p class="hero-footer">© 2026 铖信 KC-CXIM · 仅供学习交流</p>
    </section>

    <section class="login-panel">
      <div class="panel-inner">
        <div class="login-card">
          <div class="card-head">
            <div class="card-logo">
              <span>铖</span>
            </div>
            <h2>欢迎回来</h2>
            <p>登录《铖信》管理后台，开始今天的工作</p>
          </div>

          <el-form
            ref="formRef"
            :model="form"
            :rules="rules"
            label-position="top"
            size="large"
            @submit.prevent="handleLogin"
          >
            <el-form-item label="管理员账号" prop="username">
              <el-input v-model="form.username" placeholder="请输入管理员账号" clearable>
                <template #prefix>
                  <el-icon><User /></el-icon>
                </template>
              </el-input>
            </el-form-item>

            <el-form-item label="登录密码" prop="password">
              <el-input
                v-model="form.password"
                type="password"
                placeholder="请输入登录密码"
                show-password
                @keyup.enter="handleLogin"
              >
                <template #prefix>
                  <el-icon><Lock /></el-icon>
                </template>
              </el-input>
            </el-form-item>

            <div class="form-row">
              <el-checkbox v-model="form.remember">记住账号</el-checkbox>
              <button class="link-btn" type="button" @click="handleForgot">忘记密码？</button>
            </div>

            <el-button
              class="submit-btn"
              type="primary"
              size="large"
              :loading="loading"
              native-type="submit"
              @click="handleLogin"
            >
              <span v-if="!loading">进入管理后台</span>
              <span v-else>正在登录…</span>
              <el-icon v-if="!loading" class="submit-arrow"><ArrowRight /></el-icon>
            </el-button>
          </el-form>

          <div class="demo-box">
            <div class="demo-copy">
              <span class="demo-badge">DEMO</span>
              <span>演示账号：<b>admin</b> / <b>Admin@123</b></span>
            </div>
            <button class="demo-fill" type="button" @click="fillDemo">一键填充</button>
          </div>

          <p class="login-tip">
            <el-icon><InfoFilled /></el-icon>
            当前为前端骨架演示环境，尚未连接管理端 API
          </p>
        </div>

        <div class="panel-footer">
          <a href="https://github.com/fingerecho/KC-CXIM" target="_blank" rel="noreferrer">项目文档</a>
          <span>·</span>
          <span>v0.1.0</span>
          <span>·</span>
          <span>MIT License</span>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, type FormInstance, type FormRules } from 'element-plus'
import {
  User,
  Lock,
  ArrowRight,
  ChatDotRound,
  Warning,
  DataAnalysis,
  InfoFilled,
} from '@element-plus/icons-vue'
import { useAuthStore } from '@/stores/auth'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const formRef = ref<FormInstance>()
const loading = ref(false)
const form = ref({
  username: 'admin',
  password: 'Admin@123',
  remember: true,
})

const rules: FormRules = {
  username: [{ required: true, message: '请输入管理员账号', trigger: 'blur' }],
  password: [
    { required: true, message: '请输入登录密码', trigger: 'blur' },
    { min: 3, message: '密码长度至少为 3 位', trigger: 'blur' },
  ],
}

const previewBars = [38, 52, 44, 68, 58, 76, 62, 84, 72, 90, 78, 96]

function fillDemo() {
  form.value.username = 'admin'
  form.value.password = 'Admin@123'
  ElMessage.success('已填充演示账号')
}

function handleForgot() {
  ElMessage.info('演示环境：请联系超级管理员重置密码')
}

async function handleLogin() {
  if (!formRef.value) return
  const valid = await formRef.value.validate().catch(() => false)
  if (!valid) return

  loading.value = true
  try {
    await auth.login(form.value.username, form.value.password)
    ElMessage.success('登录成功，欢迎回来')
    const redirect = (route.query.redirect as string) || '/dashboard'
    router.push(redirect)
  } catch (error) {
    ElMessage.error((error as Error).message || '登录失败，请重试')
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.login-page {
  position: relative;
  min-height: 100vh;
  display: grid;
  grid-template-columns: minmax(0, 1.12fr) minmax(420px, 0.88fr);
  background: #f6f8fc;
  overflow: hidden;
}

/* ---------------- Hero ---------------- */
.login-hero {
  position: relative;
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  gap: 28px;
  padding: 52px 64px 40px;
  color: #e2e8f0;
  background:
    radial-gradient(circle at 78% 18%, rgba(37, 99, 235, 0.4), transparent 42%),
    radial-gradient(circle at 12% 82%, rgba(124, 58, 237, 0.28), transparent 45%),
    linear-gradient(150deg, #070d1a 0%, #0b1830 45%, #0f2a5c 100%);
  overflow: hidden;
}

.hero-grid {
  position: absolute;
  inset: 0;
  background-image:
    linear-gradient(rgba(148, 163, 184, 0.07) 1px, transparent 1px),
    linear-gradient(90deg, rgba(148, 163, 184, 0.07) 1px, transparent 1px);
  background-size: 54px 54px;
  mask-image: radial-gradient(circle at 50% 42%, #000 30%, transparent 78%);
  pointer-events: none;
}

.orb {
  position: absolute;
  border-radius: 50%;
  filter: blur(10px);
  pointer-events: none;
}

.orb-1 {
  width: 320px;
  height: 320px;
  top: -110px;
  right: -60px;
  background: radial-gradient(circle, rgba(56, 189, 248, 0.4), transparent 68%);
  animation: float 9s ease-in-out infinite;
}

.orb-2 {
  width: 260px;
  height: 260px;
  bottom: 12%;
  left: -90px;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.35), transparent 68%);
  animation: float 11s ease-in-out infinite reverse;
}

.orb-3 {
  width: 180px;
  height: 180px;
  top: 46%;
  right: 14%;
  background: radial-gradient(circle, rgba(6, 182, 212, 0.28), transparent 68%);
  animation: float 13s ease-in-out infinite;
}

@keyframes float {
  0%,
  100% {
    transform: translate3d(0, 0, 0);
  }
  50% {
    transform: translate3d(0, -22px, 0);
  }
}

.hero-brand {
  position: relative;
  z-index: 2;
  display: flex;
  align-items: center;
  gap: 14px;
}

.brand-mark {
  width: 46px;
  height: 46px;
  border-radius: 14px;
  display: grid;
  place-items: center;
  font-size: 22px;
  font-weight: 700;
  color: #fff;
  background: linear-gradient(135deg, #38bdf8, #2563eb 60%, #7c3aed);
  box-shadow: 0 12px 30px rgba(37, 99, 235, 0.5);
}

.brand-copy {
  display: flex;
  flex-direction: column;
  line-height: 1.2;
}

.brand-copy strong {
  font-size: 21px;
  letter-spacing: 0.14em;
  color: #f8fafc;
}

.brand-copy span {
  margin-top: 4px;
  font-size: 10px;
  letter-spacing: 0.24em;
  color: #64748b;
}

.hero-main {
  position: relative;
  z-index: 2;
  max-width: 560px;
}

.hero-eyebrow {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  margin: 0 0 20px;
  padding: 6px 12px;
  font-size: 10px;
  letter-spacing: 0.2em;
  color: #7dd3fc;
  border: 1px solid rgba(56, 189, 248, 0.24);
  border-radius: 999px;
  background: rgba(56, 189, 248, 0.08);
}

.hero-eyebrow .dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #38bdf8;
  box-shadow: 0 0 10px #38bdf8;
}

.hero-title {
  margin: 0 0 20px;
  font-size: clamp(34px, 3.5vw, 50px);
  line-height: 1.16;
  font-weight: 700;
  letter-spacing: -0.03em;
  color: #f8fafc;
}

.hero-title em {
  font-style: normal;
  background: linear-gradient(100deg, #7dd3fc 0%, #60a5fa 45%, #c4b5fd 100%);
  -webkit-background-clip: text;
  background-clip: text;
  color: transparent;
}

.hero-desc {
  margin: 0 0 26px;
  max-width: 480px;
  font-size: 14px;
  line-height: 1.85;
  color: #94a3b8;
}

.hero-features {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}

.feature-chip {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 8px 13px;
  font-size: 12px;
  color: #cbd5e1;
  border: 1px solid rgba(148, 163, 184, 0.18);
  border-radius: 10px;
  background: rgba(15, 23, 42, 0.45);
  backdrop-filter: blur(6px);
}

.feature-chip :deep(.el-icon) {
  color: #7dd3fc;
}

.hero-preview {
  position: relative;
  z-index: 2;
  width: min(100%, 520px);
  padding: 18px 20px;
  border-radius: 18px;
  background: linear-gradient(160deg, rgba(30, 41, 59, 0.72), rgba(15, 23, 42, 0.6));
  border: 1px solid rgba(148, 163, 184, 0.18);
  box-shadow: 0 24px 60px rgba(2, 6, 23, 0.45);
  backdrop-filter: blur(14px);
}

.preview-top {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
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

.preview-label {
  font-size: 12px;
  color: #cbd5e1;
  font-weight: 500;
}

.preview-live {
  margin-left: auto;
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 9px;
  letter-spacing: 0.14em;
  color: #4ade80;
}

.preview-live b {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #4ade80;
  box-shadow: 0 0 8px #4ade80;
}

.preview-stats {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 10px;
  margin-bottom: 16px;
}

.preview-stat {
  padding: 11px 12px;
  border-radius: 11px;
  background: rgba(15, 23, 42, 0.5);
  border: 1px solid rgba(148, 163, 184, 0.1);
}

.preview-stat span {
  display: block;
  font-size: 10px;
  color: #64748b;
  margin-bottom: 5px;
}

.preview-stat strong {
  font-size: 17px;
  font-weight: 650;
  color: #f1f5f9;
  letter-spacing: -0.02em;
}

.preview-stat em {
  display: inline-block;
  margin-top: 3px;
  font-size: 9px;
  font-style: normal;
  color: #4ade80;
}

.preview-chart {
  display: flex;
  align-items: flex-end;
  gap: 6px;
  height: 74px;
  padding: 0 2px 8px;
  border-bottom: 1px dashed rgba(148, 163, 184, 0.16);
  margin-bottom: 14px;
}

.preview-bar {
  flex: 1;
  border-radius: 4px 4px 2px 2px;
  background: linear-gradient(180deg, #60a5fa, rgba(37, 99, 235, 0.25));
  transition: height 0.3s ease;
}

.preview-bar:nth-child(3n) {
  background: linear-gradient(180deg, #a78bfa, rgba(124, 58, 237, 0.22));
}

.preview-foot {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  font-size: 10px;
  color: #64748b;
}

.preview-foot span {
  display: inline-flex;
  align-items: center;
  gap: 5px;
}

.preview-foot b {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.preview-foot .green {
  background: #22c55e;
}

.preview-foot .blue {
  background: #38bdf8;
}

.preview-foot .violet {
  background: #a78bfa;
}

.hero-footer {
  position: relative;
  z-index: 2;
  margin: 0;
  font-size: 11px;
  color: #475569;
  letter-spacing: 0.04em;
}

/* ---------------- Panel ---------------- */
.login-panel {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 48px 40px;
  background:
    radial-gradient(circle at 88% 8%, rgba(37, 99, 235, 0.08), transparent 34%),
    #f8fafc;
}

.panel-inner {
  width: 100%;
  max-width: 430px;
}

.login-card {
  padding: 38px 38px 32px;
  background: #fff;
  border: 1px solid #eef2f8;
  border-radius: 22px;
  box-shadow: 0 30px 80px rgba(15, 23, 42, 0.1);
}

.card-head {
  margin-bottom: 28px;
}

.card-logo {
  width: 44px;
  height: 44px;
  border-radius: 13px;
  display: grid;
  place-items: center;
  margin-bottom: 18px;
  font-size: 20px;
  font-weight: 700;
  color: #fff;
  background: linear-gradient(135deg, #38bdf8, #2563eb 62%, #7c3aed);
  box-shadow: 0 10px 24px rgba(37, 99, 235, 0.28);
}

.card-head h2 {
  margin: 0 0 8px;
  font-size: 27px;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: var(--cx-ink);
}

.card-head p {
  margin: 0;
  font-size: 13px;
  color: var(--cx-text-secondary);
}

.login-card :deep(.el-form-item) {
  margin-bottom: 20px;
}

.login-card :deep(.el-form-item__label) {
  padding-bottom: 7px;
  font-size: 13px;
  font-weight: 600;
  color: #334155;
}

.login-card :deep(.el-input__wrapper) {
  padding: 3px 14px;
  border-radius: 12px;
  background: #f6f8fc;
  box-shadow: inset 0 0 0 1px #e8edf5;
  transition: box-shadow 0.2s ease, background 0.2s ease;
}

.login-card :deep(.el-input__wrapper:hover) {
  box-shadow: inset 0 0 0 1px #cbd5e1;
}

.login-card :deep(.el-input__wrapper.is-focus) {
  background: #fff;
  box-shadow: inset 0 0 0 1px #3b82f6, 0 0 0 4px rgba(37, 99, 235, 0.1);
}

.login-card :deep(.el-input__inner) {
  height: 44px;
  font-size: 14px;
}

.login-card :deep(.el-input__prefix) {
  color: #94a3b8;
}

.form-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin: -4px 0 22px;
}

.form-row :deep(.el-checkbox__label) {
  font-size: 12px;
  color: #64748b;
}

.form-row :deep(.el-checkbox__inner) {
  border-radius: 4px;
}

.link-btn {
  border: none;
  background: transparent;
  padding: 0;
  font-size: 12px;
  color: #2563eb;
  cursor: pointer;
}

.link-btn:hover {
  text-decoration: underline;
}

.submit-btn {
  width: 100%;
  height: 48px;
  border: none;
  border-radius: 12px;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: 0.02em;
  background: linear-gradient(95deg, #2563eb, #3b82f6 55%, #6366f1);
  box-shadow: 0 14px 30px rgba(37, 99, 235, 0.32);
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}

.submit-btn:hover {
  transform: translateY(-1px);
  box-shadow: 0 18px 36px rgba(37, 99, 235, 0.4);
}

.submit-arrow {
  margin-left: 8px;
}

.demo-box {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  margin-top: 24px;
  padding: 12px 14px;
  border-radius: 12px;
  background: linear-gradient(100deg, #f0f7ff, #f5f3ff);
  border: 1px dashed #c7d2fe;
}

.demo-copy {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  color: #475569;
  flex-wrap: wrap;
}

.demo-copy b {
  color: #1d4ed8;
  font-weight: 600;
}

.demo-badge {
  padding: 2px 7px;
  border-radius: 5px;
  font-size: 9px;
  font-weight: 700;
  letter-spacing: 0.1em;
  color: #fff;
  background: linear-gradient(90deg, #6366f1, #3b82f6);
}

.demo-fill {
  flex-shrink: 0;
  border: none;
  padding: 6px 10px;
  border-radius: 7px;
  font-size: 11px;
  font-weight: 500;
  color: #1d4ed8;
  background: #fff;
  box-shadow: 0 2px 6px rgba(37, 99, 235, 0.12);
  cursor: pointer;
  transition: transform 0.2s ease;
}

.demo-fill:hover {
  transform: translateY(-1px);
}

.login-tip {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  margin: 20px 0 0;
  font-size: 11px;
  color: #94a3b8;
}

.panel-footer {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  margin-top: 24px;
  font-size: 11px;
  color: #94a3b8;
}

.panel-footer a {
  color: #64748b;
  transition: color 0.2s ease;
}

.panel-footer a:hover {
  color: #2563eb;
}

@media (max-width: 1080px) {
  .login-hero {
    padding: 44px 40px 32px;
  }
  .hero-title {
    font-size: 38px;
  }
}

@media (max-width: 900px) {
  .login-page {
    grid-template-columns: 1fr;
  }
  .login-hero {
    display: none;
  }
  .login-panel {
    min-height: 100vh;
    padding: 32px 20px;
  }
}

@media (max-width: 480px) {
  .login-card {
    padding: 28px 22px 24px;
    border-radius: 18px;
  }
  .card-head h2 {
    font-size: 23px;
  }
  .demo-box {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
