<?php

/**
 * @file
 * Drush command/hook implementation.
 */

/**
 * Implements hook_drush_command().
 */
function islandora_pdf_drush_command() {
  $commands = array();

  $commands['islandora-pdf-resanitize'] = array(
    'description' => dt('Where Solr FULL_TEXT field is missing, recreate from the Fedora datastream using new sanitize function'),
    'aliases' => array('isp-pdf-resani'),
    'options' => array(
      'pid' => array(
        'description' => dt('Specify a single PID to operate on'),
      ),
    ),
    'drupal dependencies' => array(
      'islandora',
      'islandora_solr',
    ),
    'examples' => array(
      'drush -u 1 isp-pdf-resani ' => dt('Search for Solr PIDs with sp_pdf content models and no FULL_TEXT datastream, recreate from Fedora object.'),
      'drush -u 1 isp-pdf-resani --pid="uofm:1234"' => dt('Operate on PID uofm:1234 only'),
    ),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_LOGIN,
  );
  return $commands;
}


function drush_islandora_pdf_sanitize_fn($str){
  $regex = <<<'END'
/
  (
    (?: [\x00-\x7F]                 # single-byte sequences   0xxxxxxx
    |   [\xC0-\xDF][\x80-\xBF]      # double-byte sequences   110xxxxx 10xxxxxx
    |   [\xE0-\xEF][\x80-\xBF]{2}   # triple-byte sequences   1110xxxx 10xxxxxx * 2
    |   [\xF0-\xF7][\x80-\xBF]{3}   # quadruple-byte sequence 11110xxx 10xxxxxx * 3 
    ){1,100}                        # ...one or more times
  )
| .                                 # anything else
/x
END;
  return preg_replace($regex, '$1', $str);
}

/**
 * Command callback to install required objects.
 */
function drush_islandora_pdf_resanitize() {
  module_load_include('inc', 'islandora', 'includes/utilities');
  module_load_include('php','islandora_solr','SolrPhpClient/Apache/Solr/Service');
  
  $pid_opt = drush_get_option('pid', FALSE);
  
  if ($pid_opt === FALSE) {
    $tuque = islandora_get_tuque_connection();
    $query = 'select $object from <#ri> where (
              $object <info:fedora/fedora-system:def/model#hasModel> <info:fedora/islandora:sp_pdf> and
              $object <fedora-model:state> <info:fedora/fedora-system:def/model#Active>)';
    $results = $tuque->repository->ri->itqlQuery($query, 'unlimited');
    drush_log('Checking ' . count($results) . ' pdf objects' . "\n");
    foreach ($results as $pdf_obj){
      $pid = $pdf_obj['object']['value'];
      drush_islandora_pdf_solr_check($pid);
    }
  } else {
    // Operate on a specific PID
    drush_islandora_pdf_solr_check($pid_opt);
  }
}

function drush_islandora_pdf_solr_check($pid){
  $solr_query = 'PID:"' . $pid . '"';
  $solr_params = array('fl'=>'PID,FULL_TEXT_t,FULL_TEXT2_t'); // Get the PID and dc.title
  $path_parts = parse_url(variable_get('islandora_solr_url', 'localhost:8080/solr')); 
  $solr = new Apache_Solr_Service($path_parts['host'], $path_parts['port'], $path_parts['path'] . '/');
  $solr->setCreateDocuments(0);
  $results = $solr->search($solr_query,0,1000,$solr_params);
  $reindex = FALSE;
  if ($results) {
    if (intval($results->response->numFound) == 0 || !isset($results->response->doc[0]->FULL_TEXT_t)) {
      // Not in Solr or the FULL_TEXT isn't
      $island_obj = islandora_object_load($pid);
      $reindex = drush_islandora_pdf_fix_string($island_obj,'FULL_TEXT');
      $reindex2 = drush_islandora_pdf_fix_string($island_obj,'FULL_TEXT2');
      $reindex = ($reindex || $reindex2);
      if ($reindex) {
        drush_log('Reindexing Solr record for PID (' . $pid .')');
        $resp = drush_islandora_pdf_reindex_object($pid);
        if (!$resp) {
          drush_log('Error reindexing object (' . $pid . ')','error');
        }
      }
    }
  }
}

function drush_islandora_pdf_reindex_object($pid){
  $path_parts = parse_url(variable_get('islandora_solr_url', 'localhost:8080/solr')); 
  $ch = curl_init($path_parts['host'].':'.$path_parts['port'].'/fedoragsearch/rest?operation=updateIndex&action=fromPid&value='.$pid);
  curl_setopt($ch,CURLOPT_USERPWD,'fedoraAdmin:islandorarocks');
  curl_setopt($ch,CURLOPT_RETURNTRANSFER,TRUE);
  $response = curl_exec($ch);
  curl_close($ch);
  return ($response !== FALSE);
}

function drush_islandora_pdf_fix_string(&$obj,$ds){
  if (isset($obj[$ds])){
    drush_log('Fixing OCR for datastream (' . $ds . ') in PID (' . $obj->id . ')');
    $data = $obj->getDatastream($ds);
    $full_text = $data->content;
    if (strlen(trim($full_text)) > 0) {
      $enc = mb_detect_encoding($full_text,'UTF-8,Windows-1252,ISO-8859-1,ASCII');
      $full_text = mb_convert_encoding($full_text,'UTF-8',$enc);
      
      drush_log('full_text is (' . strlen($full_text) . ') characters long');
      $full_text = islandora_sanitize_input_for_valid_xml($full_text);
      drush_log('sanitized full text is (' . strlen($full_text) . ')');
      if (strlen($full_text) == 0) {
        drush_log('islandora_sanitize_input_for_valid_xml failed with (' . drush_islandora_pdf_preg_errtxt(preg_last_error()) . '), using drush_islandora_pdf_resanitize()');
        $full_text = drush_islandora_pdf_sanitize_fn($full_text);
        if (preg_last_error() != PREG_NO_ERROR){
          drush_log('drush_islandora_pdf_sanitize_fn returned (' . drush_islandora_pdf_preg_errtxt(preg_last_error()) .')');
        }
        drush_log('drush_islandora_pdf_sanitize_fn full text is (' . strlen($full_text) . ') characters');
      }
      if (strlen($full_text) > 0 ) {
        if (preg_match("/[^\x9\xA\xD\x20-\x{D7FF}\x{E000}-\x{FFFD}\x{10000}-\x{10FFFF}]/u", $full_text,$matches)) {
          $ms = implode(', ',$matches);
          drush_log('Still found an invalid XML character in the string (' . bin2hex($ms) . ')');
        } else {
          $data->content = $full_text;
          return TRUE;
        }
      }
    }
    unset($data);
  }
  return FALSE;
}

function drush_islandora_pdf_preg_errtxt($errcode)
{
    static $errtext;

    if (!isset($errtxt))
    {
        $errtext = array();
        $constants = get_defined_constants(true);
        foreach ($constants['pcre'] as $c => $n) if (preg_match('/_ERROR$/', $c)) $errtext[$n] = $c;
    }

    return array_key_exists($errcode, $errtext)? $errtext[$errcode] : NULL;
}