import request, { type ApiResult } from './request'

export type UserStatus = 'active' | 'banned'

export interface UserItem {
  id: number
  username: string
  nickname: string
  status: UserStatus
  friends: number
  conversations: number
  messages: number
  createdAt: string
  lastActive: string
}

export interface UserPage {
  list: UserItem[]
  total: number
  page: number
  pageSize: number
}

export interface UserQuery {
  page: number
  page_size: number
  keyword?: string
  status?: 'all' | UserStatus
}

export async function fetchUsers(params: UserQuery): Promise<UserPage> {
  const response = await request.get<ApiResult<UserPage>>('/users', { params })
  return response.data.data
}

export async function updateUserStatus(
  id: number,
  status: UserStatus,
): Promise<{ id: number; status: UserStatus }> {
  const response = await request.patch<ApiResult<{ id: number; status: UserStatus }>>(
    `/users/${id}/status`,
    { status },
  )
  return response.data.data
}
