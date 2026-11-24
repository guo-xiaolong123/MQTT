<template>
	<view class="page">
		<!-- 时间范围选择 -->
		<view class="range-row">
			<view class="range-item" v-for="r in ranges" :key="r.hours"
				:class="{ 'range-active': activeRange === r.hours }"
				@click="selectRange(r.hours)">
				<text class="range-text">{{ r.label }}</text>
			</view>
		</view>

		<!-- 曲线图表 -->
		<view class="chart-card">
			<view class="legend-row">
				<view class="legend">
					<view class="legend-dot temp-dot"></view>
					<text class="legend-text">温度 (°C)</text>
				</view>
				<view class="legend">
					<view class="legend-dot humi-dot"></view>
					<text class="legend-text">湿度 (%RH)</text>
				</view>
				<text class="reset-btn" v-if="zoomed" @click="resetView()">复位视图</text>
				<text class="loading-text" v-else-if="loading">加载中...</text>
			</view>

			<!-- 数值信息栏：点按曲线后显示 -->
			<view class="tip-bar" :class="{ 'tip-bar-active': tipHas }">
				<template v-if="tipHas">
					<text class="tip-time">{{ tipTimeText }}</text>
					<view class="tip-values">
						<text class="tip-val temp-val">温度 {{ tipTempText }}°C</text>
						<text class="tip-val humi-val">湿度 {{ tipHumiText }}%</text>
					</view>
				</template>
				<text class="tip-hint" v-else>双指缩放 · 单指拖动 · 单击查看数值 · 双击复位</text>
			</view>

			<canvas canvas-id="historyChart" id="historyChart" class="chart-canvas"
				:style="{ width: canvasW + 'px', height: canvasH + 'px' }"
				:width="canvasW * dpr" :height="canvasH * dpr"
				@touchstart="onTouchStart" @touchmove.stop.prevent="onTouchMove"
				@touchend="onTouchEnd" @touchcancel="onTouchCancel"></canvas>

			<view class="empty-tip" v-if="!loading && isEmpty">
				<text class="empty-text">该时间段内暂无数据</text>
				<text class="empty-text">请确认设备已上线并上报数据</text>
			</view>
		</view>

		<!-- 统计卡片 -->
		<view class="stats-row">
			<view class="stats-card">
				<view class="stats-head">
					<image class="stats-icon" src="/static/temp.png" mode="aspectFit"></image>
					<text class="stats-title">温度统计</text>
				</view>
				<view class="stats-grid">
					<view class="stats-item">
						<text class="stats-label">当前</text>
						<text class="stats-value temp-value">{{ tempStats.cur }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">最低</text>
						<text class="stats-value temp-value">{{ tempStats.min }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">最高</text>
						<text class="stats-value temp-value">{{ tempStats.max }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">平均</text>
						<text class="stats-value temp-value">{{ tempStats.avg }}</text>
					</view>
				</view>
			</view>

			<view class="stats-card">
				<view class="stats-head">
					<image class="stats-icon" src="/static/humi.png" mode="aspectFit"></image>
					<text class="stats-title">湿度统计</text>
				</view>
				<view class="stats-grid">
					<view class="stats-item">
						<text class="stats-label">当前</text>
						<text class="stats-value humi-value">{{ humiStats.cur }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">最低</text>
						<text class="stats-value humi-value">{{ humiStats.min }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">最高</text>
						<text class="stats-value humi-value">{{ humiStats.max }}</text>
					</view>
					<view class="stats-item">
						<text class="stats-label">平均</text>
						<text class="stats-value humi-value">{{ humiStats.avg }}</text>
					</view>
				</view>
			</view>
		</view>

		<!-- 数据说明 + 刷新 -->
		<view class="meta-row">
			<text class="meta-text" v-if="pointCount > 0">共 {{ pointCount }} 个数据点 · {{ coverageText }}</text>
			<text class="meta-text" v-else>数据来源：中国移动 OneNET</text>
			<text class="refresh-btn" :class="{ 'refresh-disabled': loading }" @click="load()">刷新</text>
		</view>
	</view>
</template>

<script>
	import * as onenet from '@/common/onenet.js'

	// 最小缩放窗口(2 分钟)与最大绘制点数
	const MIN_SPAN = 2 * 60 * 1000
	const MAX_DRAW_POINTS = 600

	function pad(n) {
		return n < 10 ? '0' + n : '' + n
	}

	function fmtHM(ts) {
		const d = new Date(ts)
		return pad(d.getHours()) + ':' + pad(d.getMinutes())
	}

	function fmtHMS(ts) {
		const d = new Date(ts)
		return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds())
	}

	function fmtFullTime(ts) {
		const d = new Date(ts)
		return (d.getMonth() + 1) + '月' + d.getDate() + '日 ' + pad(d.getHours()) + ':' + pad(d.getMinutes())
	}

	/** X 轴刻度时间格式: 随视图跨度自适应 */
	function fmtTick(ts, span) {
		const d = new Date(ts)
		if (span > 12 * 3600 * 1000) {
			return (d.getMonth() + 1) + '-' + d.getDate() + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes())
		}
		if (span > 20 * 60 * 1000) {
			return pad(d.getHours()) + ':' + pad(d.getMinutes())
		}
		return fmtHMS(ts)
	}

	/** 选取合适的时间刻度步长 */
	function niceStep(span, maxTicks) {
		const steps = [1e3, 2e3, 5e3, 10e3, 15e3, 30e3, 60e3, 120e3, 300e3, 600e3, 900e3, 1800e3,
			3600e3, 7200e3, 21600e3, 43200e3, 86400e3
		]
		for (let i = 0; i < steps.length; i++) {
			if (span / steps[i] <= maxTicks) return steps[i]
		}
		return 86400e3 * 2
	}

	/** 数据量过大时做抽稀, 保证绘制流畅 */
	function downsample(arr, maxN) {
		if (!arr || arr.length <= maxN) return arr || []
		const step = arr.length / maxN
		const out = []
		for (let i = 0; i < maxN; i++) {
			const s = Math.floor(i * step)
			let e = Math.floor((i + 1) * step)
			if (e <= s) e = s + 1
			if (e > arr.length) e = arr.length
			let sum = 0
			for (let j = s; j < e; j++) sum += arr[j].value
			out.push({ time: arr[s].time, value: sum / (e - s) })
		}
		return out
	}

	/** 计算统计值 */
	function calcStats(arr) {
		if (!arr || arr.length === 0) {
			return { cur: '--', min: '--', max: '--', avg: '--' }
		}
		let min = arr[0].value
		let max = arr[0].value
		let sum = 0
		for (let i = 0; i < arr.length; i++) {
			const v = arr[i].value
			if (v < min) min = v
			if (v > max) max = v
			sum += v
		}
		const round = v => (Math.round(v * 10) / 10).toFixed(1)
		return {
			cur: round(arr[arr.length - 1].value),
			min: round(min),
			max: round(max),
			avg: round(sum / arr.length)
		}
	}

	export default {
		data() {
			return {
				ranges: [{
						label: '近1小时',
						hours: 1
					},
					{
						label: '近6小时',
						hours: 6
					},
					{
						label: '近24小时',
						hours: 24
					}
				],
				activeRange: 1,
				loading: false,
				tempData: [],
				humiData: [],
				tempStats: { cur: '--', min: '--', max: '--', avg: '--' },
				humiStats: { cur: '--', min: '--', max: '--', avg: '--' },
				pointCount: 0,
				coverageText: '',
				canvasW: 320,
				canvasH: 280,
				dpr: 2,
				lastLoadAt: 0,
				// 视图窗口(缩放/平移), null 表示全量视图
				viewT0: null,
				viewT1: null,
				// 点按十字线数值
				tipTime: null,
				tipTemp: null,
				tipHumi: null,
				lastTapAt: 0
			}
		},
		computed: {
			isEmpty() {
				return this.tempData.length === 0 && this.humiData.length === 0
			},
			zoomed() {
				return this.viewT0 !== null && this.viewT1 !== null
			},
			tipHas() {
				return this.tipTime !== null
			},
			tipTimeText() {
				return this.tipTime !== null ? fmtFullTime(this.tipTime) : ''
			},
			tipTempText() {
				return this.tipTemp !== null ? (Math.round(this.tipTemp * 10) / 10).toFixed(1) : '--'
			},
			tipHumiText() {
				return this.tipHumi !== null ? (Math.round(this.tipHumi * 10) / 10).toFixed(1) : '--'
			}
		},
		onLoad() {
			this.initCanvasSize()
		},
		onReady() {
			this.measureCanvas()
			this.load()
		},
		onShow() {
			// 页面重新可见且数据已超过 60 秒未更新时自动刷新
			if (this.lastLoadAt > 0 && Date.now() - this.lastLoadAt > 60000) {
				this.load()
			}
		},
		methods: {
			initCanvasSize() {
				const sys = uni.getSystemInfoSync()
				this.dpr = sys.pixelRatio || 2
				// 画布宽度 = 页面有效宽(最大480px) - 页面左右留白(30rpx*2) - 卡片左右内边距(24rpx*2)
				const pageW = Math.min(sys.windowWidth, 480)
				const padding = uni.upx2px(30 * 2 + 24 * 2)
				this.canvasW = pageW - padding
				this.canvasH = uni.upx2px(560)
			},
			// 测量 canvas 在页面中的位置(用于触摸坐标换算兜底)
			measureCanvas() {
				uni.createSelectorQuery().in(this).select('#historyChart').boundingClientRect(rect => {
					if (rect) this._rect = { left: rect.left, top: rect.top }
				}).exec()
			},
			selectRange(hours) {
				if (this.activeRange === hours || this.loading) return
				this.activeRange = hours
				this.load()
			},
			async load() {
				if (this.loading) return
				this.loading = true
				this.lastLoadAt = Date.now()

				const end = Date.now()
				const start = end - this.activeRange * 3600 * 1000

				try {
					const results = await Promise.all([
						onenet.queryHistory('temp', start, end),
						onenet.queryHistory('humi', start, end)
					])
					this.tempData = results[0]
					this.humiData = results[1]
					this.tempStats = calcStats(this.tempData)
					this.humiStats = calcStats(this.humiData)
					this.pointCount = this.tempData.length + this.humiData.length

					// 覆盖时间范围
					const b = this.dataBounds()
					this.coverageText = (b.t0 !== null) ? (fmtFullTime(b.t0) + ' ~ ' + fmtHM(b.t1)) : ''

					this.viewT0 = null
					this.viewT1 = null
					this.clearTip(false)

					this.$nextTick(() => {
						this.drawChart()
					})
				} catch (e) {
					uni.showToast({
						title: e.message || '查询历史数据失败',
						icon: 'none'
					})
				} finally {
					this.loading = false
				}
			},

			/* ==================== 视图与坐标 ==================== */
			// 数据整体时间范围
			dataBounds() {
				let t0 = null
				let t1 = null
				if (this.tempData.length > 0) {
					t0 = this.tempData[0].time
					t1 = this.tempData[this.tempData.length - 1].time
				}
				if (this.humiData.length > 0) {
					const h0 = this.humiData[0].time
					const h1 = this.humiData[this.humiData.length - 1].time
					t0 = t0 === null ? h0 : Math.min(t0, h0)
					t1 = t1 === null ? h1 : Math.max(t1, h1)
				}
				if (t0 !== null && t1 <= t0) t1 = t0 + 1000
				return { t0: t0, t1: t1 }
			},
			// 当前视图时间范围
			currentView() {
				if (this.viewT0 !== null) return { t0: this.viewT0, t1: this.viewT1 }
				const b = this.dataBounds()
				return { t0: b.t0, t1: b.t1 }
			},
			// 绘图几何参数
			geo() {
				return {
					padL: 40,
					padR: 44,
					padT: 14,
					padB: 26,
					plotW: this.canvasW - 40 - 44,
					plotH: this.canvasH - 14 - 26
				}
			},
			timeToX(t) {
				const v = this.currentView()
				const g = this.geo()
				return g.padL + (t - v.t0) / (v.t1 - v.t0) * g.plotW
			},
			xToTime(x) {
				const v = this.currentView()
				const g = this.geo()
				return v.t0 + (x - g.padL) / g.plotW * (v.t1 - v.t0)
			},
			// 设置视图窗口(带边界与最小跨度约束)
			setView(t0, t1) {
				const b = this.dataBounds()
				if (b.t0 === null) return
				let span = t1 - t0
				if (span < MIN_SPAN) {
					const c = (t0 + t1) / 2
					t0 = c - MIN_SPAN / 2
					t1 = c + MIN_SPAN / 2
					span = MIN_SPAN
				}
				if (span >= b.t1 - b.t0) {
					this.viewT0 = null
					this.viewT1 = null
					this.drawChart()
					return
				}
				if (t0 < b.t0) {
					t0 = b.t0
					t1 = t0 + span
				}
				if (t1 > b.t1) {
					t1 = b.t1
					t0 = t1 - span
				}
				this.viewT0 = t0
				this.viewT1 = t1
				this.drawChart()
			},
			resetView() {
				this.viewT0 = null
				this.viewT1 = null
				this.clearTip(true)
			},

			/* ==================== 触摸手势 ==================== */
			touchX(t) {
				if (t && typeof t.x === 'number') return t.x
				if (t && typeof t.clientX === 'number' && this._rect) return t.clientX - this._rect.left
				return 0
			},
			onTouchStart(e) {
				const ts = e.touches
				if (ts.length >= 2) {
					// 双指: 缩放手势
					const x1 = this.touchX(ts[0])
					const x2 = this.touchX(ts[1])
					const v = this.currentView()
					this._g = {
						mode: 'pinch',
						d0: Math.max(Math.abs(x2 - x1), 10),
						span0: v.t1 - v.t0,
						tc: this.xToTime((x1 + x2) / 2)
					}
					this.clearTip(false)
				} else if (ts.length === 1) {
					// 单指: 可能是拖动或点按
					const x = this.touchX(ts[0])
					this._g = {
						mode: 'pan',
						startX: x,
						lastX: x,
						moved: false
					}
				}
			},
			onTouchMove(e) {
				const g = this._g
				if (!g) return
				const ts = e.touches

				if (g.mode === 'pinch' && ts.length >= 2) {
					const x1 = this.touchX(ts[0])
					const x2 = this.touchX(ts[1])
					const d1 = Math.max(Math.abs(x2 - x1), 10)
					const midX = (x1 + x2) / 2
					// 手指张合比例换算窗口跨度
					let span = g.span0 * g.d0 / d1
					const b = this.dataBounds()
					if (b.t0 !== null) {
						span = Math.max(MIN_SPAN, Math.min(b.t1 - b.t0, span))
						const geo = this.geo()
						const f = Math.min(Math.max((midX - geo.padL) / geo.plotW, 0), 1)
						this.setView(g.tc - f * span, g.tc + (1 - f) * span)
					}
				} else if (g.mode === 'pan' && ts.length >= 1) {
					const x = this.touchX(ts[0])
					if (Math.abs(x - g.startX) > 8) {
						if (!g.moved) {
							g.moved = true
							this.clearTip(false)
						}
						const v = this.currentView()
						const geo = this.geo()
						const dt = -(x - g.lastX) / geo.plotW * (v.t1 - v.t0)
						this.setView(v.t0 + dt, v.t1 + dt)
					}
					g.lastX = x
				}
			},
			onTouchEnd(e) {
				const g = this._g
				if (!g) return

				if (g.mode === 'pinch') {
					// 双指抬起来还剩一根: 转为单指拖动
					if (e.touches.length === 1) {
						const x = this.touchX(e.touches[0])
						this._g = { mode: 'pan', startX: x, lastX: x, moved: true }
					} else if (e.touches.length === 0) {
						this._g = null
					}
					return
				}

				if (g.mode === 'pan' && !g.moved) {
					// 未移动 → 视为点按
					const now = Date.now()
					if (now - this.lastTapAt < 300) {
						// 双击复位
						this.lastTapAt = 0
						this.resetView()
					} else {
						this.lastTapAt = now
						this.showTipAt(g.startX)
					}
				}
				if (e.touches.length === 0) this._g = null
			},
			onTouchCancel() {
				this._g = null
			},

			/* ==================== 十字线数值 ==================== */
			clearTip(redraw) {
				const had = this.tipTime !== null
				this.tipTime = null
				this.tipTemp = null
				this.tipHumi = null
				if (had && redraw !== false) this.drawChart()
			},
			// 在某个 x 位置查找最近的数据点并显示十字线
			showTipAt(x) {
				const b = this.dataBounds()
				if (b.t0 === null) return
				const t = this.xToTime(x)
				if (t < b.t0 || t > b.t1) return

				const self = this

				function nearest(arr) {
					if (!arr || arr.length === 0) return null
					let best = null
					let bestD = Infinity
					for (let i = 0; i < arr.length; i++) {
						const d = Math.abs(arr[i].time - t)
						if (d < bestD) {
							bestD = d
							best = arr[i]
						}
					}
					return best
				}

				const tp = nearest(this.tempData)
				const hp = nearest(this.humiData)
				if (!tp && !hp) return

				// 十字线取两组数据中更接近点按位置的时间
				let tipT = t
				if (tp && hp) tipT = Math.abs(tp.time - t) <= Math.abs(hp.time - t) ? tp.time : hp.time
				else if (tp) tipT = tp.time
				else tipT = hp.time

				this.tipTime = tipT
				this.tipTemp = tp ? tp.value : null
				this.tipHumi = hp ? hp.value : null
				this.drawChart()
			},

			/* ==================== 绘制 ==================== */
			// 取时间窗口内的数据片段(向两端各扩一个点保证曲线连贯)
			visibleSlice(arr, t0, t1) {
				if (!arr || arr.length === 0) return []
				const out = []
				for (let i = 0; i < arr.length; i++) {
					if (arr[i].time >= t0 && arr[i].time <= t1) out.push(arr[i])
				}
				if (out.length === 0) return out
				// 向两端各补一个相邻点, 曲线画出边界保持连贯
				const idxFirst = arr.indexOf(out[0])
				const idxLast = arr.indexOf(out[out.length - 1])
				if (idxFirst > 0) out.unshift(arr[idxFirst - 1])
				if (idxLast >= 0 && idxLast < arr.length - 1) out.push(arr[idxLast + 1])
				return out
			},
			drawChart() {
				const ctx = uni.createCanvasContext('historyChart', this)
				const W = this.canvasW
				const H = this.canvasH
				const dpr = this.dpr

				ctx.scale(dpr, dpr)
				ctx.clearRect(0, 0, W, H)

				const b = this.dataBounds()
				if (b.t0 === null) {
					ctx.draw()
					return
				}
				const v = this.currentView()
				const g = this.geo()
				const span = v.t1 - v.t0

				// 当前窗口内的数据(抽稀后用于绘制)
				const temp = downsample(this.visibleSlice(this.tempData, v.t0, v.t1), MAX_DRAW_POINTS)
				const humi = downsample(this.visibleSlice(this.humiData, v.t0, v.t1), MAX_DRAW_POINTS)

				// Y 轴范围
				function numRange(arr) {
					let min = Infinity
					let max = -Infinity
					for (let i = 0; i < arr.length; i++) {
						if (arr[i].value < min) min = arr[i].value
						if (arr[i].value > max) max = arr[i].value
					}
					if (min === Infinity) return null
					if (max - min < 0.4) {
						const c = (max + min) / 2
						min = c - 1
						max = c + 1
					}
					const margin = (max - min) * 0.12
					return { min: min - margin, max: max + margin }
				}
				const tr = numRange(temp) || { min: 0, max: 1 }
				const hr = numRange(humi) || { min: 0, max: 1 }

				const yOf = (val, r) => g.padT + (1 - (val - r.min) / (r.max - r.min)) * g.plotH
				const xOf = t => g.padL + (t - v.t0) / span * g.plotW

				/* ---------- 网格与坐标轴 ---------- */
				if (ctx.setLineDash) ctx.setLineDash([3, 3])
				ctx.setStrokeStyle('#eef1f6')
				ctx.setLineWidth(0.5)
				ctx.setFontSize(9)
				const ROWS = 4
				for (let i = 0; i <= ROWS; i++) {
					const y = g.padT + g.plotH * (1 - i / ROWS)
					ctx.beginPath()
					ctx.moveTo(g.padL, y)
					ctx.lineTo(g.padL + g.plotW, y)
					ctx.stroke()
					// 左轴: 温度 / 右轴: 湿度
					ctx.setFillStyle('#c3cad6')
					ctx.fillText((tr.min + (tr.max - tr.min) * i / ROWS).toFixed(1), 2, y + 3)
					ctx.fillText((hr.min + (hr.max - hr.min) * i / ROWS).toFixed(0), W - 30, y + 3)
				}

				// X 轴: 时间刻度(取整数步长, 从整时刻开始)
				const step = niceStep(span, 4)
				if (ctx.setLineDash) ctx.setLineDash([3, 3])
				let t = Math.ceil(v.t0 / step) * step
				while (t <= v.t1) {
					const x = xOf(t)
					ctx.setStrokeStyle('#eef1f6')
					ctx.beginPath()
					ctx.moveTo(x, g.padT)
					ctx.lineTo(x, g.padT + g.plotH)
					ctx.stroke()

					const label = fmtTick(t, span)
					const tw = label.length * 5
					ctx.setFillStyle('#c3cad6')
					ctx.fillText(label, Math.min(Math.max(x - tw / 2, 2), W - tw - 2), H - 8)
					t += step
				}
				if (ctx.setLineDash) ctx.setLineDash([])

				/* ---------- 曲线绘制 ---------- */
				function buildPts(arr, r) {
					const pts = []
					for (let i = 0; i < arr.length; i++) {
						const x = xOf(arr[i].time)
						if (x < g.padL - 20 || x > g.padL + g.plotW + 20) continue
						pts.push({ x: x, y: yOf(arr[i].value, r) })
					}
					return pts
				}

				function linePath(pts) {
					ctx.beginPath()
					for (let i = 0; i < pts.length; i++) {
						if (i === 0) {
							ctx.moveTo(pts[0].x, pts[0].y)
						} else if (pts.length >= 3) {
							// 中点二次贝塞尔平滑
							const xc = (pts[i].x + pts[i - 1].x) / 2
							const yc = (pts[i].y + pts[i - 1].y) / 2
							ctx.quadraticCurveTo(pts[i - 1].x, pts[i - 1].y, xc, yc)
						} else {
							ctx.lineTo(pts[i].x, pts[i].y)
						}
					}
					if (pts.length > 0) ctx.lineTo(pts[pts.length - 1].x, pts[pts.length - 1].y)
				}

				function drawSeries(arr, r, color, fillTop) {
					const pts = buildPts(arr, r)
					if (pts.length === 0) return
					const bottom = g.padT + g.plotH

					// 渐变面积填充
					if (pts.length > 1 && ctx.createLinearGradient) {
						const grad = ctx.createLinearGradient(0, g.padT, 0, bottom)
						grad.addColorStop(0, fillTop)
						grad.addColorStop(1, 'rgba(255,255,255,0)')
						ctx.setFillStyle(grad)
						ctx.beginPath()
						ctx.moveTo(pts[0].x, bottom)
						for (let i = 0; i < pts.length; i++) ctx.lineTo(pts[i].x, pts[i].y)
						ctx.lineTo(pts[pts.length - 1].x, bottom)
						ctx.closePath()
						ctx.fill()
					}

					// 平滑折线
					ctx.setStrokeStyle(color)
					ctx.setLineWidth(2)
					ctx.setLineJoin('round')
					ctx.setLineCap('round')
					linePath(pts)
					ctx.stroke()

					// 数据点少时画上圆点
					if (pts.length > 1 && pts.length <= 60) {
						ctx.setFillStyle(color)
						for (let i = 0; i < pts.length; i++) {
							ctx.beginPath()
							ctx.arc(pts[i].x, pts[i].y, 2, 0, Math.PI * 2)
							ctx.fill()
						}
					}
				}

				drawSeries(humi, hr, '#3d8cff', 'rgba(61,140,255,0.16)')
				drawSeries(temp, tr, '#ff6b4a', 'rgba(255,107,74,0.16)')

				/* ---------- 十字线提示 ---------- */
				if (this.tipTime !== null && this.tipTime >= v.t0 && this.tipTime <= v.t1) {
					const cx = xOf(this.tipTime)
					if (ctx.setLineDash) ctx.setLineDash([4, 3])
					ctx.setStrokeStyle('#8a93a6')
					ctx.setLineWidth(0.8)
					ctx.beginPath()
					ctx.moveTo(cx, g.padT)
					ctx.lineTo(cx, g.padT + g.plotH)
					ctx.stroke()
					if (ctx.setLineDash) ctx.setLineDash([])

					// 交点圆点
					function drawMark(arr, r, color) {
						if (!arr || arr.length === 0) return
						let best = null
						let bestD = Infinity
						for (let i = 0; i < arr.length; i++) {
							const d = Math.abs(arr[i].time - self.tipTime)
							if (d < bestD) {
								bestD = d
								best = arr[i]
							}
						}
						if (!best) return
						const y = yOf(best.value, r)
						ctx.setFillStyle('#ffffff')
						ctx.setStrokeStyle(color)
						ctx.setLineWidth(2)
						ctx.beginPath()
						ctx.arc(cx, y, 4, 0, Math.PI * 2)
						ctx.fill()
						ctx.stroke()
					}
					const self = this
					drawMark(this.tempData, tr, '#ff6b4a')
					drawMark(this.humiData, hr, '#3d8cff')
				}

				ctx.draw()
			}
		}
	}
</script>

<style>
	.page {
		min-height: 100vh;
		max-width: 480px;
		margin: 0 auto;
		background: linear-gradient(180deg, #eef2fb 0%, #f6f8fc 40%, #f6f8fc 100%);
		padding: 30rpx 30rpx 60rpx;
		box-sizing: border-box;
	}

	/* ---------- 时间范围选择 ---------- */
	.range-row {
		display: flex;
		background: #ffffff;
		border-radius: 18rpx;
		padding: 8rpx;
		box-shadow: 0 6rpx 20rpx rgba(60, 80, 140, 0.06);
	}

	.range-item {
		flex: 1;
		display: flex;
		align-items: center;
		justify-content: center;
		height: 64rpx;
		border-radius: 14rpx;
	}

	.range-active {
		background: linear-gradient(135deg, #4a6cf7 0%, #6a5cff 100%);
		box-shadow: 0 4rpx 12rpx rgba(74, 108, 247, 0.35);
	}

	.range-text {
		font-size: 26rpx;
		color: #7a8395;
	}

	.range-active .range-text {
		color: #ffffff;
		font-weight: bold;
	}

	/* ---------- 图表卡片 ---------- */
	.chart-card {
		position: relative;
		margin-top: 24rpx;
		background: #ffffff;
		border-radius: 24rpx;
		padding: 24rpx;
		box-shadow: 0 6rpx 20rpx rgba(60, 80, 140, 0.06);
	}

	.legend-row {
		display: flex;
		align-items: center;
		padding: 4rpx 4rpx 12rpx;
	}

	.legend {
		display: flex;
		align-items: center;
		margin-right: 28rpx;
	}

	.legend-dot {
		width: 14rpx;
		height: 14rpx;
		border-radius: 50%;
		margin-right: 10rpx;
	}

	.temp-dot {
		background: #ff6b4a;
	}

	.humi-dot {
		background: #3d8cff;
	}

	.legend-text {
		font-size: 22rpx;
		color: #7a8395;
	}

	.loading-text {
		margin-left: auto;
		font-size: 22rpx;
		color: #4a6cf7;
	}

	.reset-btn {
		margin-left: auto;
		font-size: 22rpx;
		color: #4a6cf7;
		padding: 6rpx 20rpx;
		border: 2rpx solid #4a6cf7;
		border-radius: 999rpx;
	}

	/* ---------- 数值信息栏 ---------- */
	.tip-bar {
		display: flex;
		align-items: center;
		min-height: 64rpx;
		border-radius: 14rpx;
		background: #f6f8fc;
		padding: 8rpx 20rpx;
		margin-bottom: 12rpx;
	}

	.tip-bar-active {
		background: linear-gradient(135deg, rgba(74, 108, 247, 0.10), rgba(106, 92, 255, 0.10));
	}

	.tip-time {
		font-size: 24rpx;
		color: #20242b;
		font-weight: bold;
	}

	.tip-values {
		display: flex;
		margin-left: auto;
	}

	.tip-val {
		font-size: 22rpx;
		margin-left: 20rpx;
		font-weight: bold;
	}

	.temp-val {
		color: #ff6b4a;
	}

	.humi-val {
		color: #3d8cff;
	}

	.tip-hint {
		font-size: 20rpx;
		color: #b4bcc9;
	}

	.chart-canvas {
		display: block;
		width: 100%;
	}

	.empty-tip {
		position: absolute;
		top: 50%;
		left: 50%;
		transform: translate(-50%, -50%);
		display: flex;
		flex-direction: column;
		align-items: center;
		justify-content: center;
	}

	.empty-text {
		font-size: 24rpx;
		color: #b4bcc9;
		text-align: center;
		line-height: 40rpx;
	}

	/* ---------- 统计卡片 ---------- */
	.stats-row {
		display: flex;
		margin-top: 24rpx;
	}

	.stats-card {
		flex: 1;
		background: #ffffff;
		border-radius: 24rpx;
		padding: 26rpx 24rpx;
		box-shadow: 0 6rpx 20rpx rgba(60, 80, 140, 0.06);
	}

	.stats-card + .stats-card {
		margin-left: 22rpx;
	}

	.stats-head {
		display: flex;
		align-items: center;
	}

	.stats-icon {
		width: 44rpx;
		height: 44rpx;
	}

	.stats-title {
		margin-left: 12rpx;
		font-size: 26rpx;
		font-weight: bold;
		color: #20242b;
	}

	.stats-grid {
		display: flex;
		flex-wrap: wrap;
		margin-top: 16rpx;
	}

	.stats-item {
		width: 50%;
		display: flex;
		flex-direction: column;
		margin: 10rpx 0;
	}

	.stats-label {
		font-size: 20rpx;
		color: #9aa3b2;
	}

	.stats-value {
		margin-top: 4rpx;
		font-size: 30rpx;
		font-weight: bold;
	}

	.temp-value {
		color: #ff6b4a;
	}

	.humi-value {
		color: #3d8cff;
	}

	/* ---------- 底部 ---------- */
	.meta-row {
		display: flex;
		align-items: center;
		justify-content: space-between;
		margin-top: 24rpx;
		padding: 0 6rpx;
	}

	.meta-text {
		font-size: 22rpx;
		color: #9aa3b2;
	}

	.refresh-btn {
		font-size: 24rpx;
		color: #4a6cf7;
		padding: 8rpx 28rpx;
		border: 2rpx solid #4a6cf7;
		border-radius: 999rpx;
	}

	.refresh-disabled {
		opacity: 0.4;
	}
</style>
