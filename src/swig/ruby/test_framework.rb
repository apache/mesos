#!/usr/bin/env ruby

require 'nexus'

# TODO: Ruby schedulers seems to be slightly broken under the new API - find out why
class MyScheduler < Nexus::Scheduler
  def get_framework_name(driver)
    "Ruby test framework"
  end

  def get_executor_info(driver)
    Nexus::ExecutorInfo.new(Dir.pwd + "/../../test-executor", "")
  end

  def registered(driver, fid)
    puts "Registered with framework ID #{fid}"
  end

  def resource_offer(driver, oid, offers)
    puts "Got resource offer #{oid}!"
    puts "Hosts in offer: #{offers.map{|x| x.host}.join(', ')}"
    driver.reply_to_offer(oid, [], {"timeout" => "1"})
  end
end

sched = MyScheduler.new
driver = Nexus::NexusSchedulerDriver.new(sched, ARGV[0])
driver.run()
