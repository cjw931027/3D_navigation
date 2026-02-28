import { defineStore } from 'pinia'
import { ref } from 'vue'


// 定義「座標點」必須包含 x 和 y 兩個數字
export interface Point {
  x: number;
  y: number;
}

export const useMapStore = defineStore('map', () => {
  
  // --- A. 存放資料的地方 (State) ---
  
  // 存放圖片的原始像素資料 (用來給後面的 C++ 演算法計算)
  // Uint8ClampedArray 是瀏覽器專門用來存圖片像素的格式
  const imageRawData = ref<Uint8ClampedArray | null>(null)
  
  // 地圖的寬度與高度
  const mapWidth = ref<number>(0)
  const mapHeight = ref<number>(0)
  
  // 使用者點擊的起點與終點座標
  const startPoint = ref<Point | null>(null)
  const endPoint = ref<Point | null>(null)

  // --- B. 修改資料的方法 (Actions) ---

  // 當使用者上傳圖片並解析完畢後，呼叫這個方法把資料存起來
  function setMapData(data: Uint8ClampedArray, width: number, height: number) {
    imageRawData.value = data
    mapWidth.value = width
    mapHeight.value = height
  }

  // 當使用者在地圖上點擊起點或終點時，呼叫這個方法存座標
  function setPoints(start: Point | null, end: Point | null) {
    startPoint.value = start
    endPoint.value = end
  }

  // --- C. 開放給外面使用的東西 (Return) ---
  // 只有寫在這裡面的東西，其他的 .vue 頁面才能拿得到
  return {
    imageRawData,
    mapWidth,
    mapHeight,
    startPoint,
    endPoint,
    setMapData,
    setPoints
  }
})