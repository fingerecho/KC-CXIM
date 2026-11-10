import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'
import { useAuthStore } from '@/stores/auth'

const routes: RouteRecordRaw[] = [
  {
    path: '/login',
    name: 'login',
    component: () => import('@/views/login/LoginView.vue'),
    meta: { title: '登录', public: true },
  },
  {
    path: '/',
    component: () => import('@/layout/MainLayout.vue'),
    redirect: '/dashboard',
    children: [
      {
        path: 'dashboard',
        name: 'dashboard',
        component: () => import('@/views/dashboard/DashboardView.vue'),
        meta: { title: '工作台', subtitle: '掌握铖信运行状态与核心运营数据' },
      },
      {
        path: 'users',
        name: 'users',
        component: () => import('@/views/users/UserListView.vue'),
        meta: { title: '用户管理', subtitle: '查询用户资料、管理账号状态与安全信息' },
      },
      {
        path: 'friends',
        name: 'friends',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '好友关系',
          subtitle: '关系链与黑名单管理',
          description: '查询用户好友关系、解除好友、维护黑名单，保障关系链健康。',
          features: ['好友关系检索', '申请中 / 已好友 / 拉黑状态', '双向解除好友', '关系详情与共同会话'],
        },
      },
      {
        path: 'groups',
        name: 'groups',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '群组管理',
          subtitle: '群聊运营与成员治理',
          description: '管理群资料、成员、禁言与入群审批，待后端群组能力就绪后开放。',
          features: ['群列表与详情', '成员与角色管理', '禁言 / 踢人 / 黑名单', '群主转让与解散'],
        },
      },
      {
        path: 'messages',
        name: 'messages',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '消息管理',
          subtitle: '会话检索与消息治理',
          description: '按会话、用户、时间与类型检索消息，并支持撤回、删除等治理动作。',
          features: ['多条件消息检索', '会话上下文视图', '消息撤回 / 删除', '审计与操作留痕'],
        },
      },
      {
        path: 'moderation',
        name: 'moderation',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '内容审核',
          subtitle: '敏感词与举报处理',
          description: '维护敏感词库、处理用户举报，形成违规内容处置闭环。',
          features: ['举报队列', '敏感词库', '审核记录', '违规内容下架'],
        },
      },
      {
        path: 'push',
        name: 'push',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '推送管理',
          subtitle: '模板、人群与定时任务',
          description: '配置推送模板、选择目标人群、查看推送记录，依赖推送服务上线。',
          features: ['推送模板', '人群选择', '定时推送', '推送记录与频控'],
        },
      },
      {
        path: 'files',
        name: 'files',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '文件管理',
          subtitle: '对象存储资源治理',
          description: '查询、清理对象存储文件，管理用户与全局存储配额。',
          features: ['文件检索', '无效文件清理', '配额管理', '下载鉴权'],
        },
      },
      {
        path: 'config',
        name: 'config',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '系统配置',
          subtitle: '开关、限流与公告',
          description: '在线调整功能开关、限流参数、全站公告与客户端版本配置。',
          features: ['功能开关', '限流参数', '全站公告', '变更审计与回滚'],
        },
      },
      {
        path: 'audit',
        name: 'audit',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '审计日志',
          subtitle: '管理操作全量留痕',
          description: '记录管理员的关键操作，可筛选、可导出、不可篡改。',
          features: ['操作留痕', '多条件筛选', '审计导出', '高风险操作追踪'],
        },
      },
      {
        path: 'admins',
        name: 'admins',
        component: () => import('@/views/common/ComingSoonView.vue'),
        meta: {
          title: '管理员与角色',
          subtitle: 'RBAC 权限体系',
          description: '维护管理员账号、角色与权限点，落实最小权限原则。',
          features: ['管理员账号', '角色管理', '权限分配', '登录安全策略'],
        },
      },
    ],
  },
  {
    path: '/:pathMatch(.*)*',
    redirect: '/dashboard',
  },
]

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

router.beforeEach((to) => {
  const auth = useAuthStore()
  if (to.meta.public) {
    if (auth.isAuthenticated && to.path === '/login') {
      return { path: '/dashboard' }
    }
    return true
  }
  if (!auth.isAuthenticated) {
    return { path: '/login', query: { redirect: to.fullPath } }
  }
  return true
})

router.afterEach((to) => {
  const title = to.meta.title as string | undefined
  document.title = title ? `${title} · 铖信管理后台` : '铖信 · 管理后台'
})

export default router
