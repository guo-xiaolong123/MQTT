<script>
	export default {
		onLaunch: function() {
			console.log('App Launch')
		},
		onShow: function() {
			console.log('App Show')
		},
		onHide: function() {
			console.log('App Hide')
		}
	}
</script>

<style>
	/*每个页面公共css */
	/* #ifdef H5 */
	/* 桌面浏览器打开时, 页面居中显示为手机宽度, 两侧加衬底色 */
	body {
		background: #e2e8f5;
	}
	/* #endif */
</style>
