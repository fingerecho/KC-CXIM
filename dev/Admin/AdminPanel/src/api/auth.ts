import request, { type ApiResult } from './request'

export interface AdminProfile {
  id: number
  username: string
  name: string
  role: string
  roleName: string
}

export interface LoginData {
  token: string
  refreshToken: string
  admin: AdminProfile
}

export async function loginApi(username: string, password: string): Promise<LoginData> {
  const response = await request.post<ApiResult<LoginData>>('/auth/login', {
    username,
    password,
  })
  return response.data.data
}
