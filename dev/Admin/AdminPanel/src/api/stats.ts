import request, { type ApiResult } from './request'

export interface StatsOverview {
  onlineUsers: number
  onlineUsersAvailable: boolean
  todayMessages: number
  activeUsers: number
  newUsers: number
}

export interface TrendData {
  dates: string[]
  messages: number[]
  actives: number[]
}

export async function fetchStatsOverview(): Promise<StatsOverview> {
  const response = await request.get<ApiResult<StatsOverview>>('/stats/overview')
  return response.data.data
}

export async function fetchStatsTrend(days: 7 | 30): Promise<TrendData> {
  const response = await request.get<ApiResult<TrendData>>('/stats/trend', {
    params: { days },
  })
  return response.data.data
}
