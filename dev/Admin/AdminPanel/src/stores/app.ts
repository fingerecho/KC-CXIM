import { ref } from 'vue'
import { defineStore } from 'pinia'

const COLLAPSE_KEY = 'cx_admin_sidebar_collapsed'

export const useAppStore = defineStore('app', () => {
  const collapsed = ref(localStorage.getItem(COLLAPSE_KEY) === '1')

  function toggleCollapse() {
    collapsed.value = !collapsed.value
    localStorage.setItem(COLLAPSE_KEY, collapsed.value ? '1' : '0')
  }

  return { collapsed, toggleCollapse }
})
