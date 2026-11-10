import { computed, ref } from 'vue'
import { defineStore } from 'pinia'
import { loginApi } from '@/api/auth'

const TOKEN_KEY = 'cx_admin_token'
const REFRESH_TOKEN_KEY = 'cx_admin_refresh_token'
const USER_KEY = 'cx_admin_user'

export interface AdminUser {
  id: number
  username: string
  name: string
  role: string
  roleName: string
  avatar?: string
}

function readUser(): AdminUser | null {
  try {
    const raw = localStorage.getItem(USER_KEY)
    return raw ? (JSON.parse(raw) as AdminUser) : null
  } catch {
    return null
  }
}

export const useAuthStore = defineStore('auth', () => {
  const token = ref<string>(localStorage.getItem(TOKEN_KEY) || '')
  const user = ref<AdminUser | null>(readUser())

  const isAuthenticated = computed(() => Boolean(token.value))
  const displayName = computed(() => user.value?.name || user.value?.username || '管理员')
  const initial = computed(() => displayName.value.slice(0, 1).toUpperCase())

  async function login(username: string, password: string) {
    if (!username.trim() || !password.trim()) {
      throw new Error('请输入管理员账号和密码')
    }

    try {
      const data = await loginApi(username.trim(), password)
      token.value = data.token
      user.value = data.admin
      localStorage.setItem(TOKEN_KEY, data.token)
      localStorage.setItem(REFRESH_TOKEN_KEY, data.refreshToken)
      localStorage.setItem(USER_KEY, JSON.stringify(data.admin))
    } catch (error) {
      const message =
        (error as { response?: { data?: { message?: string } } })?.response?.data?.message ||
        (error as Error)?.message ||
        '登录失败，请重试'
      throw new Error(message)
    }
  }

  function logout() {
    token.value = ''
    user.value = null
    localStorage.removeItem(TOKEN_KEY)
    localStorage.removeItem(REFRESH_TOKEN_KEY)
    localStorage.removeItem(USER_KEY)
  }

  return { token, user, isAuthenticated, displayName, initial, login, logout }
})
