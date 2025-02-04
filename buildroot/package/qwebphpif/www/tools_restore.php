#!/usr/lib/cgi-bin/php-cgi

<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN"  "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" lang="en" xml:lang="en">
<head>
	<title>Quantenna Communications</title>
	<link rel="stylesheet" type="text/css" href="./themes/style.css" media="screen" />

	<meta http-equiv="Content-Type" content="text/html; charset=UTF-8" />
	<meta http-equiv="expires" content="0" />
	<meta http-equiv="CACHE-CONTROL" content="no-cache" />
</head>
<script language="javascript" type="text/javascript" src="./js/menu.js"></script>
<script language="javascript" type="text/javascript" src="./js/webif.js"></script>
<?php
include("common.php");
$privilege = get_privilege(0);

?>


<script type="text/javascript">
var privilege="<?php echo $privilege; ?>";
</script>

<?php
$curr_mode=exec("call_qcsapi get_mode wifi0");

if (isset($_POST['btn_yes']) or isset($_POST['btn_keep_ip'])) {
	if (!(isset($_POST['csrf_token']) && $_POST['csrf_token'] === get_session_token())) {
		header('Location: login.php');
		exit();
	}

	if (isset($_POST['btn_keep_ip'])) {
		exec("/scripts/restore_default_config -nr");
	} else {
		exec("/scripts/restore_default_config -nr -ip");
	}
	$_SESSION['qtn_can_reboot']=TRUE;
	header('Location: system_rebooted.php');
	exit();
}
?>

<body class="body" onload="init_menu();">
	<div class="top">
		<a class="logo" href="./status_device.php">
			<img src="./images/logo.png"/>
		</a>
	</div>
<form enctype="multipart/form-data" action="tools_restore.php" id="mainform" name="mainform" method="post">
<div class="container">
	<div class="left">
		<script type="text/javascript">
			createMenu('<?php echo $curr_mode;?>',privilege);
		</script>
	</div>
	<div class="right">
	<div class="righttop">TOOLS - RESTORE</div>
		<div class="rightmain">
			<table class="tablemain">
				<tr>
					<td>Restore all configuration files to factory defaults and reboot?&nbsp;&nbsp;
						<input type="submit" name="btn_yes" id="btn_yes" value="YES" class="button"/>
					</td>
				</tr>
				<tr>
					<td><br /><br /></td>
				</tr>
				<tr>
					<td>Restore configuration files to default and reboot, but retain IP settings?&nbsp;&nbsp;
						<input type="submit" name="btn_keep_ip" id="btn_keep_ip" value="YES" class="button"/>
					</td>
				</tr>
			</table>
		</div>
	</div>
</div>
<input type="hidden" name="csrf_token" value="<?php echo get_session_token(); ?>" />
</form>
<div class="bottom">
	<a href="help/aboutus.php">About Quantenna</a> |  <a href="help/contactus.php">Contact Us</a> | <a href="help/privacypolicy.php">Privacy Policy</a> | <a href="help/terms.php">Terms of Use</a> <br />
	<div><?php echo $str_copy ?></div>
</div>
</body>
</html>
