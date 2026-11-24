<template>
	<view class="page">
		<!-- 顶部设备信息卡片 -->
		<view class="header-card">
			<view class="header-left">
				<image class="header-icon" src="/static/home.png" mode="aspectFit"></image>
				<view class="header-info">
					<text class="header-title">全屋智能家居</text>
					<text class="header-sub">设备 HJ_Family · OneNET</text>
				</view>
			</view>
			<view class="status-pill" :class="online ? 'status-online' : 'status-offline'">
				<view class="status-dot"></view>
				<text class="status-text">{{ online ? '在线' : '离线' }}</text>
			</view>
		</view>

		<!-- 温湿度卡片 -->
		<view class="sensor-row">
			<view class="sensor-card">
				<view class="sensor-icon-wrap temp-wrap">
					<image class="sensor-icon" src="/static/temp.png" mode="aspectFit"></image>
				</view>
				<view class="sensor-info">
					<text class="sensor-label">温度</text>
					<view class="sensor-value-row">
						<text class="sensor-value">{{ temp === null ? '--' : temp }}</text>
						<text class="sensor-unit">°C</text>
					</view>
				</view>
			</view>
			<view class="sensor-card">
				<view class="sensor-icon-wrap humi-wrap">
					<image class="sensor-icon" src="/static/humi.png" mode="aspectFit"></image>
				</view>
				<view class="sensor-info">
					<text class="sensor-label">湿度</text>
					<view class="sensor-value-row">
						<text class="sensor-value">{{ humi === null ? '--' : humi }}</text>
						<text class="sensor-unit">%RH</text>
					</view>
				</view>
			</view>
		</view>

		<!-- 继电器控制 -->
		<view class="section-row">
			<text class="section-title">设备控制</text>
			<text class="section-sub">更新于 {{ lastUpdate || '--:--:--' }}</text>
		</view>
		<view class="relay-grid">
			<view class="relay-card" v-for="item in relays" :key="item.id"
				:class="{ 'relay-active': item.on }"
				:style="item.on ? { background: item.bg, borderColor: item.color } : {}"
				@click="toggleRelay(item)">
				<view class="relay-icon-wrap" :style="item.on ? { background: item.color } : {}">
					<image class="relay-icon" :src="item.icon" mode="aspectFit"></image>
				</view>
				<text class="relay-name" :style="item.on ? { color: '#20242b' } : {}">{{ item.name }}</text>
				<text class="relay-state" :style="item.on ? { color: item.color } : {}">{{ item.on ? '已开启' : '已关闭' }}</text>
				<view class="toggle" :class="{ 'toggle-on': item.on }">
					<view class="knob"></view>
				</view>
			</view>
		</view>

		<text class="footer">数据来源：中国移动 OneNET · 每 5 秒自动刷新</text>
	</view>
</template>

<script>
	import * as onenet from '@/common/onenet.js'

	function pad(n) {
		return n < 10 ? '0' + n : '' + n
	}

	export default {
		data() {
			return {
				online: false,
				temp: null,
				humi: null,
				lastUpdate: '',
				relays: [{
						id: 'relay1',
						name: '插座',
						icon: '/static/socket.png',
						color: '#ffb020',
						bg: 'rgba(255,176,32,0.10)',
						on: false,
						busy: false
					},
					{
						id: 'relay2',
						name: '灯光',
						icon: '/static/light.png',
						color: '#8f7bff',
						bg: 'rgba(143,123,255,0.10)',
						on: false,
						busy: false
					},
					{
						id: 'relay3',
						name: '风扇',
						icon: '/static/fan.png',
						color: '#2bc3a4',
						bg: 'rgba(43,195,164,0.10)',
						on: false,
						busy: false
					},
					{
						id: 'relay4',
						name: '热水器',
						icon: '/static/heater.png',
						color: '#ff6b5e',
						bg: 'rgba(255,107,94,0.10)',
						on: false,
						busy: false
					}
				],
				timer: null
			}
		},
		onLoad() {
			this.startPoll()
		},
		onShow() {
			this.startPoll()
		},
		onHide() {
			this.stopPoll()
		},
		onUnload() {
			this.stopPoll()
		},
		methods: {
			startPoll() {
				if (this.timer) return
				this.fetchData()
				this.timer = setInterval(() => {
					this.fetchData()
				}, 5000)
			},
			stopPoll() {
				if (this.timer) {
					clearInterval(this.timer)
					this.timer = null
				}
			},
			async fetchData() {
				try {
					const results = await Promise.all([onenet.getDeviceStatus(), onenet.getLatestProperties()])
					const status = results[0]
					const props = results[1] || []

					this.online = !!(status && status.online)

					const now = new Date()
					this.lastUpdate = pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds())

					for (let i = 0; i < props.length; i++) {
						const p = props[i]
						if (p.identifier === 'temp') {
							const v = parseFloat(p.value)
							this.temp = isNaN(v) ? null : (Math.round(v * 10) / 10)
						} else if (p.identifier === 'humi') {
							const v = parseFloat(p.value)
							this.humi = isNaN(v) ? null : (Math.round(v * 10) / 10)
						} else {
							const r = this.findRelay(p.identifier)
							// 正在下发指令时不被轮询覆盖
							if (r && !r.busy) {
								r.on = (p.value === 'true')
							}
						}
					}
				} catch (e) {
					this.online = false
				}
			},
			findRelay(id) {
				for (let i = 0; i < this.relays.length; i++) {
					if (this.relays[i].id === id) return this.relays[i]
				}
				return null
			},
			async toggleRelay(item) {
				if (item.busy) return
				if (!this.online) {
					uni.showToast({
						title: '设备离线，无法控制',
						icon: 'none'
					})
					return
				}

				item.busy = true
				const target = !item.on
				item.on = target // 先乐观更新界面

				try {
					const params = {}
					params[item.id] = target
					await onenet.setProperty(params)
					uni.showToast({
						title: (target ? '已开启' : '已关闭') + item.name,
						icon: 'none'
					})
				} catch (e) {
					item.on = !target // 失败回滚
					uni.showToast({
						title: e.message || '设置失败',
						icon: 'none'
					})
				} finally {
					item.busy = false
				}
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

	/* ---------- 顶部卡片 ---------- */
	.header-card {
		display: flex;
		align-items: center;
		justify-content: space-between;
		background: linear-gradient(135deg, #4a6cf7 0%, #6a5cff 100%);
		border-radius: 28rpx;
		padding: 36rpx 32rpx;
		box-shadow: 0 10rpx 30rpx rgba(74, 108, 247, 0.28);
	}

	.header-left {
		display: flex;
		align-items: center;
	}

	.header-icon {
		width: 96rpx;
		height: 96rpx;
		border-radius: 22rpx;
		background: rgba(255, 255, 255, 0.22);
		padding: 10rpx;
		box-sizing: border-box;
	}

	.header-info {
		margin-left: 24rpx;
		display: flex;
		flex-direction: column;
	}

	.header-title {
		font-size: 36rpx;
		font-weight: bold;
		color: #ffffff;
	}

	.header-sub {
		margin-top: 8rpx;
		font-size: 22rpx;
		color: rgba(255, 255, 255, 0.85);
	}

	.status-pill {
		display: flex;
		align-items: center;
		padding: 10rpx 22rpx;
		border-radius: 999rpx;
		background: rgba(255, 255, 255, 0.2);
	}

	.status-dot {
		width: 14rpx;
		height: 14rpx;
		border-radius: 50%;
		margin-right: 10rpx;
	}

	.status-online .status-dot {
		background: #2bff9d;
		box-shadow: 0 0 10rpx #2bff9d;
	}

	.status-offline .status-dot {
		background: #ffb020;
		box-shadow: 0 0 10rpx #ffb020;
	}

	.status-text {
		font-size: 22rpx;
		color: #ffffff;
	}

	/* ---------- 温湿度卡片 ---------- */
	.sensor-row {
		display: flex;
		margin-top: 28rpx;
	}

	.sensor-card {
		flex: 1;
		display: flex;
		align-items: center;
		background: #ffffff;
		border-radius: 24rpx;
		padding: 30rpx 26rpx;
		box-shadow: 0 6rpx 20rpx rgba(60, 80, 140, 0.06);
	}

	.sensor-card + .sensor-card {
		margin-left: 22rpx;
	}

	.sensor-icon-wrap {
		width: 88rpx;
		height: 88rpx;
		border-radius: 20rpx;
		display: flex;
		align-items: center;
		justify-content: center;
	}

	.temp-wrap {
		background: rgba(255, 107, 74, 0.10);
	}

	.humi-wrap {
		background: rgba(61, 140, 255, 0.10);
	}

	.sensor-icon {
		width: 56rpx;
		height: 56rpx;
	}

	.sensor-info {
		margin-left: 20rpx;
		display: flex;
		flex-direction: column;
	}

	.sensor-label {
		font-size: 24rpx;
		color: #9aa3b2;
	}

	.sensor-value-row {
		display: flex;
		align-items: baseline;
		margin-top: 6rpx;
	}

	.sensor-value {
		font-size: 48rpx;
		font-weight: bold;
		color: #20242b;
	}

	.sensor-unit {
		margin-left: 8rpx;
		font-size: 22rpx;
		color: #9aa3b2;
	}

	/* ---------- 继电器网格 ---------- */
	.section-row {
		display: flex;
		align-items: baseline;
		justify-content: space-between;
		margin-top: 40rpx;
		margin-bottom: 20rpx;
	}

	.section-title {
		font-size: 32rpx;
		font-weight: bold;
		color: #20242b;
	}

	.section-sub {
		font-size: 22rpx;
		color: #9aa3b2;
	}

	.relay-grid {
		display: flex;
		flex-wrap: wrap;
		justify-content: space-between;
	}

	.relay-card {
		width: calc(50% - 11rpx);
		box-sizing: border-box;
		display: flex;
		flex-direction: column;
		align-items: center;
		background: #ffffff;
		border-radius: 24rpx;
		border: 2rpx solid transparent;
		padding: 36rpx 24rpx 30rpx;
		margin-bottom: 22rpx;
		box-shadow: 0 6rpx 20rpx rgba(60, 80, 140, 0.06);
	}

	.relay-active {
		box-shadow: 0 8rpx 26rpx rgba(60, 80, 140, 0.14);
	}

	.relay-icon-wrap {
		width: 108rpx;
		height: 108rpx;
		border-radius: 26rpx;
		background: #f1f4f9;
		display: flex;
		align-items: center;
		justify-content: center;
	}

	.relay-icon {
		width: 68rpx;
		height: 68rpx;
	}

	.relay-name {
		margin-top: 20rpx;
		font-size: 28rpx;
		font-weight: bold;
		color: #20242b;
	}

	.relay-state {
		margin-top: 8rpx;
		font-size: 22rpx;
		color: #9aa3b2;
	}

	/* ---------- 自定义开关 ---------- */
	.toggle {
		margin-top: 24rpx;
		width: 92rpx;
		height: 48rpx;
		border-radius: 999rpx;
		background: #dfe4ee;
		padding: 4rpx;
		box-sizing: border-box;
		transition: background 0.25s;
	}

	.toggle-on {
		background: #4a6cf7;
	}

	.knob {
		width: 40rpx;
		height: 40rpx;
		border-radius: 50%;
		background: #ffffff;
		box-shadow: 0 2rpx 8rpx rgba(0, 0, 0, 0.18);
		transform: translateX(0);
		transition: transform 0.25s;
	}

	.toggle-on .knob {
		transform: translateX(44rpx);
	}

	/* ---------- 底部 ---------- */
	.footer {
		display: block;
		text-align: center;
		margin-top: 20rpx;
		font-size: 22rpx;
		color: #b4bcc9;
	}
</style>
